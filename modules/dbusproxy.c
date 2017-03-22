/**
   @file dbusproxy.c

   This module implements proxying of between DSME's internal message
   queue and D-Bus.
   <p>
   Copyright (C) 2009-2010 Nokia Corporation.
   Copyright (C) 2015-2017 Jolla Ltd.

   @author Semi Malinen <semi.malinen@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * An example command line to obtain dsme version number over D-Bus:
 * $ dbus-send --system --print-reply --dest=com.nokia.dsme /com/nokia/dsme com.nokia.dsme.request.get_version
 *
 * TODO:
 * - dsme should cope with D-Bus restarts
 */
#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>
#include <dsme/dsme_dbus_if.h>

#include <glib.h>
#include <stdlib.h>

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

static const char *shutdown_action_name    (dsme_state_t state);
static const char *state_name              (dsme_state_t state);
static void        get_version             (const DsmeDbusMessage *request, DsmeDbusMessage* *reply);
static void        get_state               (const DsmeDbusMessage *request, DsmeDbusMessage* *reply);
static void        req_powerup             (const DsmeDbusMessage *request, DsmeDbusMessage* *reply);
static void        req_reboot              (const DsmeDbusMessage *request, DsmeDbusMessage* *reply);
static void        req_shutdown            (const DsmeDbusMessage *request, DsmeDbusMessage* *reply);
static void        emit_dsme_dbus_signal   (const char *name);
static void        emit_dsme_state_signals (void);
void               module_init             (module_t *handle);
void               module_fini             (void);

/* ========================================================================= *
 * DATA
 * ========================================================================= */

/** Cached dsme version string */
static char* dsme_version = 0;

/** Cache dsme state */
static int dsme_state = DSME_STATE_NOT_SET;

/** Cached dbus connection state */
static bool dbus_connected = false;

/* ========================================================================= *
 * FUNCTIONS
 * ========================================================================= */

static void get_version(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
  *reply = dsme_dbus_reply_new(request);
  dsme_dbus_message_append_string(*reply,
                                  dsme_version ? dsme_version : "unknown");
}

static void get_state(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
    *reply = dsme_dbus_reply_new(request);
    dsme_dbus_message_append_string(*reply, state_name(dsme_state));
}

static void req_powerup(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
  char* sender = dsme_dbus_endpoint_name(request);
  dsme_log(LOG_NOTICE,
           "powerup request received over D-Bus from %s",
           sender ? sender : "(unknown)");
  free(sender);

  DSM_MSGTYPE_POWERUP_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);
  broadcast_internally(&req);
}

static void req_reboot(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
  char* sender = dsme_dbus_endpoint_name(request);
  dsme_log(LOG_NOTICE,
           "reboot request received over D-Bus from %s",
           sender ? sender : "(unknown)");
  free(sender);

  DSM_MSGTYPE_REBOOT_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);
  broadcast_internally(&req);
}

static void req_shutdown(const DsmeDbusMessage* request,
                         DsmeDbusMessage**      reply)
{
  char* sender = dsme_dbus_endpoint_name(request);
  dsme_log(LOG_NOTICE,
           "shutdown request received over D-Bus from %s",
           sender ? sender : "(unknown)");
  free(sender);

  DSM_MSGTYPE_SHUTDOWN_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

  broadcast_internally(&req);
}

/** Flag for: dbus broadcast info has been installed */
static bool dbus_broadcast_bound           = false;

/** Array of signals that can be broadcast */
static const dsme_dbus_binding_t dbus_broadcast_array[] =
{
    // outbound signals
    {
        .name   = dsme_state_change_ind,
        .args   =
            "    <arg name=\"state\" type=\"s\"/>\n"
    },
    {
        .name   = dsme_save_unsaved_data_ind,
        .args   = ""
    },
    {
        .name   = dsme_battery_empty_ind,
        .args   = ""
    },
    {
        .name   = dsme_thermal_shutdown_ind,
        .args   = ""
    },
    {
        .name   = dsme_shutdown_ind,
        .args   = ""
    },
    {
        .name   = dsme_state_req_denied_ind,
        .args   =
            "    <arg name=\"denied_state\" type=\"s\"/>\n"
            "    <arg name=\"reason\" type=\"s\"/>\n"
    },
    // sentinel
    {
        .name   = 0,
    },
};

/** Array of dsme method call handlers */
static const dsme_dbus_binding_t dbus_methods_array[] =
{
    // method calls
    {
        .method = get_version,
        .name   = dsme_get_version,
        .args   =
            "    <arg direction=\"out\" name=\"version\" type=\"s\"/>\n"
    },
    {
        .method = get_state,
        .name   = dsme_get_state,
        .args   =
            "    <arg direction=\"out\" name=\"state\" type=\"s\"/>\n"
    },
    {
        .method = req_powerup,
        .name   = dsme_req_powerup,
        .args   = ""
    },
    {
        .method = req_reboot,
        .name   = dsme_req_reboot,
        .args   = ""
    },
    {
        .method = req_shutdown,
        .name   = dsme_req_shutdown,
        .args   = ""
    },
    // sentinel
    {
        .name   = 0,
    }
};

/** Flag for: dbus method call handlers have been installed */
static bool dbus_methods_bound = false;

static const char* shutdown_action_name(dsme_state_t state)
{
    return (state == DSME_STATE_REBOOT ? "reboot" : "shutdown");
}

static const struct {
    int         value;
    const char* name;
} states[] = {
#define DSME_STATE(STATE, VALUE) { VALUE, #STATE },
#include <dsme/state_states.h>
#undef  DSME_STATE
};

static const char* state_name(dsme_state_t state)
{
    int         index;
    const char* name = "UNKNOWN";;

    for (index = 0; index < sizeof states / sizeof states[0]; ++index) {
        if (states[index].value == state) {
            name = states[index].name;
            break;
        }
    }

    return name;
}

static void emit_dsme_dbus_signal(const char* name)
{
  DsmeDbusMessage* sig = dsme_dbus_signal_new(dsme_service, dsme_sig_path, dsme_sig_interface, name);
  dsme_dbus_signal_emit(sig);
}

static void emit_dsme_state_signals(void)
{
    if( dsme_state == DSME_STATE_NOT_SET )
        goto EXIT;

    if( !dbus_connected )
        goto EXIT;

    switch( dsme_state ) {
    case DSME_STATE_SHUTDOWN:
    case DSME_STATE_ACTDEAD:
    case DSME_STATE_REBOOT:
        emit_dsme_dbus_signal(dsme_shutdown_ind);
        break;

    default:
        break;
    }

    DsmeDbusMessage* sig = dsme_dbus_signal_new(dsme_service, dsme_sig_path,
                                                dsme_sig_interface,
                                                dsme_state_change_ind);
    dsme_dbus_message_append_string(sig, state_name(dsme_state));
    dsme_dbus_signal_emit(sig);

EXIT:
    return;
}

DSME_HANDLER(DSM_MSGTYPE_STATE_CHANGE_IND, server, msg)
{
    if( dsme_state == msg->state )
        goto EXIT;

    dsme_state = msg->state;

    emit_dsme_state_signals();

EXIT:
    return;
}

DSME_HANDLER(DSM_MSGTYPE_BATTERY_EMPTY_IND, server, msg)
{
  emit_dsme_dbus_signal(dsme_battery_empty_ind);
}

DSME_HANDLER(DSM_MSGTYPE_SET_THERMAL_STATUS, server, msg)
{
  if( msg->status == DSM_THERMAL_STATUS_OVERHEATED ) {
    emit_dsme_dbus_signal(dsme_thermal_shutdown_ind);
  }
}

DSME_HANDLER(DSM_MSGTYPE_SAVE_DATA_IND, server, msg)
{
  emit_dsme_dbus_signal(dsme_save_unsaved_data_ind);
}

DSME_HANDLER(DSM_MSGTYPE_STATE_REQ_DENIED_IND, server, msg)
{
    const char* denied_request = shutdown_action_name(msg->state);

    dsme_log(LOG_WARNING,
             "proxying %s request denial due to %s to D-Bus",
             denied_request,
             (const char*)DSMEMSG_EXTRA(msg));

    DsmeDbusMessage* sig = dsme_dbus_signal_new(dsme_service, dsme_sig_path,
                                                dsme_sig_interface,
                                                dsme_state_req_denied_ind);
    dsme_dbus_message_append_string(sig, denied_request);
    dsme_dbus_message_append_string(sig, DSMEMSG_EXTRA(msg));

    dsme_dbus_signal_emit(sig);
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, "dbusproxy: DBUS_CONNECT");

  dsme_dbus_bind_methods(&dbus_broadcast_bound,
                         dsme_service,
                         dsme_sig_path,
                         dsme_sig_interface,
                         dbus_broadcast_array);

  dsme_dbus_bind_methods(&dbus_methods_bound,
                         dsme_service,
                         dsme_req_path,
                         dsme_req_interface,
                         dbus_methods_array);
  dsme_dbus_connect();
  dbus_connected = true;

  emit_dsme_state_signals();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, "dbusproxy: DBUS_DISCONNECT");
  dsme_dbus_disconnect();
  dbus_connected = false;
}

DSME_HANDLER(DSM_MSGTYPE_DSME_VERSION, server, msg)
{
  if (!dsme_version) {
      dsme_version = g_strdup(DSMEMSG_EXTRA(msg));
  }
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_CHANGE_IND),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_BATTERY_EMPTY_IND),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_THERMAL_STATUS),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_SAVE_DATA_IND),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_REQ_DENIED_IND),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECT),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DSME_VERSION),
  { 0 }
};

void module_init(module_t* handle)
{
  /* get dsme version so that we can report it over D-Bus if asked to */
  DSM_MSGTYPE_GET_VERSION req_version = DSME_MSG_INIT(DSM_MSGTYPE_GET_VERSION);
  broadcast_internally(&req_version);

  /* get dsme state so that we can report it over D-Bus if asked to */
  DSM_MSGTYPE_STATE_QUERY req_state = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);
  broadcast_internally(&req_state);

  /* Enable dbus functionality */
  dsme_dbus_startup();

  /* Do not connect to D-Bus; it is probably not started yet.
   * Instead, wait for DSM_MSGTYPE_DBUS_CONNECT.
   */

  dsme_log(LOG_DEBUG, "dbusproxy.so loaded");
}

void module_fini(void)
{
    dsme_dbus_unbind_methods(&dbus_broadcast_bound,
                             dsme_service,
                             dsme_sig_path,
                             dsme_sig_interface,
                             dbus_broadcast_array);

    dsme_dbus_unbind_methods(&dbus_methods_bound,
                             dsme_service,
                             dsme_req_path,
                             dsme_req_interface,
                             dbus_methods_array);

    dsme_dbus_shutdown();

    g_free(dsme_version);
    dsme_version = 0;

    dsme_log(LOG_DEBUG, "dbusproxy.so unloaded");
}
