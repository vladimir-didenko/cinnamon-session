/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus-glib.h>

#include "cs-idle-monitor.h"

#include "csm-presence.h"
#include "csm-presence-glue.h"

#define CSM_PRESENCE_DBUS_PATH "/org/cinnamon/SessionManager/Presence"

#define CS_NAME      "org.cinnamon.ScreenSaver"
#define CS_PATH      "/org/cinnamon/ScreenSaver"
#define CS_INTERFACE "org.cinnamon.ScreenSaver"

#define MAX_STATUS_TEXT 140

#define CSM_PRESENCE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CSM_TYPE_PRESENCE, CsmPresencePrivate))

struct CsmPresencePrivate
{
        guint            status;
        guint            saved_status;
        char            *status_text;
        gboolean         idle_enabled;
        CSIdleMonitor   *idle_monitor;
        guint            idle_watch_id;
        guint            idle_timeout;
        gboolean         screensaver_active;
        DBusGConnection *bus_connection;
        DBusGProxy      *bus_proxy;
        DBusGProxy      *screensaver_proxy;
};

enum {
        PROP_0,
        PROP_STATUS,
        PROP_STATUS_TEXT,
        PROP_IDLE_ENABLED,
        PROP_IDLE_TIMEOUT,
};


enum {
        STATUS_CHANGED,
        STATUS_TEXT_CHANGED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (CsmPresence, csm_presence, G_TYPE_OBJECT)

GQuark
csm_presence_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("csm_presence_error");
        }

        return ret;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

GType
csm_presence_error_get_type (void)
{
        static GType etype = 0;

        if (etype == 0) {
                static const GEnumValue values[] = {
                        ENUM_ENTRY (CSM_PRESENCE_ERROR_GENERAL, "GeneralError"),
                        { 0, 0, 0 }
                };

                g_assert (CSM_PRESENCE_NUM_ERRORS == G_N_ELEMENTS (values) - 1);

                etype = g_enum_register_static ("CsmPresenceError", values);
        }

        return etype;
}

static void
set_session_idle (CsmPresence   *presence,
                  gboolean       is_idle)
{
        g_debug ("CsmPresence: setting idle: %d", is_idle);

        if (is_idle) {
                if (presence->priv->status == CSM_PRESENCE_STATUS_IDLE) {
                        g_debug ("CsmPresence: already idle, ignoring");
                        return;
                }

                /* save current status */
                presence->priv->saved_status = presence->priv->status;
                csm_presence_set_status (presence, CSM_PRESENCE_STATUS_IDLE, NULL);
        } else {
                if (presence->priv->status != CSM_PRESENCE_STATUS_IDLE) {
                        g_debug ("CsmPresence: already not idle, ignoring");
                        return;
                }

                /* restore saved status */
                csm_presence_set_status (presence, presence->priv->saved_status, NULL);
                presence->priv->saved_status = CSM_PRESENCE_STATUS_AVAILABLE;
        }
}

static gboolean
on_idle_timeout (CSIdleMonitor *monitor,
                 guint          id,
                 gboolean       condition,
                 CsmPresence   *presence)
{
        gboolean handled;

        handled = TRUE;
        set_session_idle (presence, condition);

        return handled;
}

static void
reset_idle_watch (CsmPresence  *presence)
{
        if (presence->priv->idle_monitor == NULL) {
                return;
        }

        if (presence->priv->idle_watch_id > 0) {
                g_debug ("CsmPresence: removing idle watch");
                cs_idle_monitor_remove_watch (presence->priv->idle_monitor,
                                              presence->priv->idle_watch_id);
                presence->priv->idle_watch_id = 0;
        }

        if (! presence->priv->screensaver_active
            && presence->priv->idle_enabled
            && presence->priv->idle_timeout > 0) {
                g_debug ("CsmPresence: adding idle watch");

                presence->priv->idle_watch_id = cs_idle_monitor_add_watch (presence->priv->idle_monitor,
                                                                           presence->priv->idle_timeout,
                                                                           (CSIdleMonitorWatchFunc)on_idle_timeout,
                                                                           presence);
        }
}

static void
on_screensaver_active_changed (DBusGProxy  *proxy,
                               gboolean     is_active,
                               CsmPresence *presence)
{
        g_debug ("screensaver status changed: %d", is_active);
        if (presence->priv->screensaver_active != is_active) {
                presence->priv->screensaver_active = is_active;
                reset_idle_watch (presence);
                set_session_idle (presence, is_active);
        }
}

static void
on_screensaver_proxy_destroy (GObject     *proxy,
                              CsmPresence *presence)
{
        g_warning ("Detected that screensaver has left the bus");

        presence->priv->screensaver_proxy = NULL;
        presence->priv->screensaver_active = FALSE;
        set_session_idle (presence, FALSE);
        reset_idle_watch (presence);
}

static void
on_bus_name_owner_changed (DBusGProxy  *bus_proxy,
                           const char  *service_name,
                           const char  *old_service_name,
                           const char  *new_service_name,
                           CsmPresence *presence)
{
        GError *error;

        if (service_name == NULL
            || strcmp (service_name, CS_NAME) != 0) {
                /* ignore */
                return;
        }

        if (strlen (new_service_name) == 0
            && strlen (old_service_name) > 0) {
                /* service removed */
                /* let destroy signal handle this? */
        } else if (strlen (old_service_name) == 0
                   && strlen (new_service_name) > 0) {
                /* service added */
                error = NULL;
                presence->priv->screensaver_proxy = dbus_g_proxy_new_for_name_owner (presence->priv->bus_connection,
                                                                                     CS_NAME,
                                                                                     CS_PATH,
                                                                                     CS_INTERFACE,
                                                                                     &error);
                if (presence->priv->screensaver_proxy != NULL) {
                        g_signal_connect (presence->priv->screensaver_proxy,
                                          "destroy",
                                          G_CALLBACK (on_screensaver_proxy_destroy),
                                          presence);
                        dbus_g_proxy_add_signal (presence->priv->screensaver_proxy,
                                                 "ActiveChanged",
                                                 G_TYPE_BOOLEAN,
                                                 G_TYPE_INVALID);
                        dbus_g_proxy_connect_signal (presence->priv->screensaver_proxy,
                                                     "ActiveChanged",
                                                     G_CALLBACK (on_screensaver_active_changed),
                                                     presence,
                                                     NULL);
                } else {
                        g_warning ("Unable to get screensaver proxy: %s", error->message);
                        g_error_free (error);
                }
        }
}

static gboolean
register_presence (CsmPresence *presence)
{
        GError *error;

        error = NULL;
        presence->priv->bus_connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
        if (presence->priv->bus_connection == NULL) {
                if (error != NULL) {
                        g_critical ("error getting session bus: %s", error->message);
                        g_error_free (error);
                }
                return FALSE;
        }

        dbus_g_connection_register_g_object (presence->priv->bus_connection, CSM_PRESENCE_DBUS_PATH, G_OBJECT (presence));

        return TRUE;
}

static GObject *
csm_presence_constructor (GType                  type,
                          guint                  n_construct_properties,
                          GObjectConstructParam *construct_properties)
{
        CsmPresence *presence;
        gboolean     res;

        presence = CSM_PRESENCE (G_OBJECT_CLASS (csm_presence_parent_class)->constructor (type,
                                                                                             n_construct_properties,
                                                                                             construct_properties));

        res = register_presence (presence);
        if (! res) {
                g_warning ("Unable to register presence with session bus");
        }

        presence->priv->bus_proxy = dbus_g_proxy_new_for_name (presence->priv->bus_connection,
                                                               DBUS_SERVICE_DBUS,
                                                               DBUS_PATH_DBUS,
                                                               DBUS_INTERFACE_DBUS);
        if (presence->priv->bus_proxy != NULL) {
                dbus_g_proxy_add_signal (presence->priv->bus_proxy,
                                         "NameOwnerChanged",
                                         G_TYPE_STRING,
                                         G_TYPE_STRING,
                                         G_TYPE_STRING,
                                         G_TYPE_INVALID);
                dbus_g_proxy_connect_signal (presence->priv->bus_proxy,
                                             "NameOwnerChanged",
                                             G_CALLBACK (on_bus_name_owner_changed),
                                             presence,
                                             NULL);
        }

        return G_OBJECT (presence);
}

static void
csm_presence_init (CsmPresence *presence)
{
        presence->priv = CSM_PRESENCE_GET_PRIVATE (presence);

        presence->priv->idle_monitor = cs_idle_monitor_new ();
}

void
csm_presence_set_idle_enabled (CsmPresence  *presence,
                               gboolean      enabled)
{
        g_return_if_fail (CSM_IS_PRESENCE (presence));

        if (presence->priv->idle_enabled != enabled) {
                presence->priv->idle_enabled = enabled;
                reset_idle_watch (presence);
                g_object_notify (G_OBJECT (presence), "idle-enabled");

        }
}

gboolean
csm_presence_set_status_text (CsmPresence  *presence,
                              const char   *status_text,
                              GError      **error)
{
        g_return_val_if_fail (CSM_IS_PRESENCE (presence), FALSE);

        g_free (presence->priv->status_text);

        /* check length */
        if (status_text != NULL && strlen (status_text) > MAX_STATUS_TEXT) {
                g_set_error (error,
                             CSM_PRESENCE_ERROR,
                             CSM_PRESENCE_ERROR_GENERAL,
                             "Status text too long");
                return FALSE;
        }

        if (status_text != NULL) {
                presence->priv->status_text = g_strdup (status_text);
        } else {
                presence->priv->status_text = g_strdup ("");
        }
        g_object_notify (G_OBJECT (presence), "status-text");
        g_signal_emit (presence, signals[STATUS_TEXT_CHANGED], 0, presence->priv->status_text);
        return TRUE;
}

gboolean
csm_presence_set_status (CsmPresence  *presence,
                         guint         status,
                         GError      **error)
{
        g_return_val_if_fail (CSM_IS_PRESENCE (presence), FALSE);

        if (status != presence->priv->status) {
                presence->priv->status = status;
                g_object_notify (G_OBJECT (presence), "status");
                g_signal_emit (presence, signals[STATUS_CHANGED], 0, presence->priv->status);
        }
        return TRUE;
}

void
csm_presence_set_idle_timeout (CsmPresence  *presence,
                               guint         timeout)
{
        g_return_if_fail (CSM_IS_PRESENCE (presence));

        if (timeout != presence->priv->idle_timeout) {
                presence->priv->idle_timeout = timeout;
                reset_idle_watch (presence);
                g_object_notify (G_OBJECT (presence), "idle-timeout");
        }
}

static void
csm_presence_set_property (GObject       *object,
                           guint          prop_id,
                           const GValue  *value,
                           GParamSpec    *pspec)
{
        CsmPresence *self;

        self = CSM_PRESENCE (object);

        switch (prop_id) {
        case PROP_STATUS:
                csm_presence_set_status (self, g_value_get_uint (value), NULL);
                break;
        case PROP_STATUS_TEXT:
                csm_presence_set_status_text (self, g_value_get_string (value), NULL);
                break;
        case PROP_IDLE_ENABLED:
                csm_presence_set_idle_enabled (self, g_value_get_boolean (value));
                break;
        case PROP_IDLE_TIMEOUT:
                csm_presence_set_idle_timeout (self, g_value_get_uint (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
csm_presence_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
        CsmPresence *self;

        self = CSM_PRESENCE (object);

        switch (prop_id) {
        case PROP_STATUS:
                g_value_set_uint (value, self->priv->status);
                break;
        case PROP_STATUS_TEXT:
                g_value_set_string (value, self->priv->status_text);
                break;
        case PROP_IDLE_ENABLED:
                g_value_set_boolean (value, self->priv->idle_enabled);
                break;
        case PROP_IDLE_TIMEOUT:
                g_value_set_uint (value, self->priv->idle_timeout);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
csm_presence_finalize (GObject *object)
{
        CsmPresence *presence = (CsmPresence *) object;

        if (presence->priv->idle_watch_id > 0) {
                cs_idle_monitor_remove_watch (presence->priv->idle_monitor,
                                              presence->priv->idle_watch_id);
                presence->priv->idle_watch_id = 0;
        }

        if (presence->priv->status_text != NULL) {
                g_free (presence->priv->status_text);
                presence->priv->status_text = NULL;
        }

        if (presence->priv->idle_monitor != NULL) {
                g_object_unref (presence->priv->idle_monitor);
                presence->priv->idle_monitor = NULL;
        }

        G_OBJECT_CLASS (csm_presence_parent_class)->finalize (object);
}

static void
csm_presence_class_init (CsmPresenceClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize             = csm_presence_finalize;
        object_class->constructor          = csm_presence_constructor;
        object_class->get_property         = csm_presence_get_property;
        object_class->set_property         = csm_presence_set_property;

        signals [STATUS_CHANGED] =
                g_signal_new ("status-changed",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (CsmPresenceClass, status_changed),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__UINT,
                              G_TYPE_NONE,
                              1, G_TYPE_UINT);
        signals [STATUS_TEXT_CHANGED] =
                g_signal_new ("status-text-changed",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (CsmPresenceClass, status_text_changed),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__STRING,
                              G_TYPE_NONE,
                              1, G_TYPE_STRING);

        g_object_class_install_property (object_class,
                                         PROP_STATUS,
                                         g_param_spec_uint ("status",
                                                            "status",
                                                            "status",
                                                            0,
                                                            G_MAXINT,
                                                            0,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_STATUS_TEXT,
                                         g_param_spec_string ("status-text",
                                                              "status text",
                                                              "status text",
                                                              "",
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_IDLE_ENABLED,
                                         g_param_spec_boolean ("idle-enabled",
                                                               NULL,
                                                               NULL,
                                                               FALSE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_IDLE_TIMEOUT,
                                         g_param_spec_uint ("idle-timeout",
                                                            "idle timeout",
                                                            "idle timeout",
                                                            0,
                                                            G_MAXINT,
                                                            300000,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

        dbus_g_object_type_install_info (CSM_TYPE_PRESENCE, &dbus_glib_csm_presence_object_info);
        dbus_g_error_domain_register (CSM_PRESENCE_ERROR, NULL, CSM_PRESENCE_TYPE_ERROR);
        g_type_class_add_private (klass, sizeof (CsmPresencePrivate));
}

CsmPresence *
csm_presence_new (void)
{
        CsmPresence *presence;

        presence = g_object_new (CSM_TYPE_PRESENCE,
                                 NULL);

        return presence;
}
