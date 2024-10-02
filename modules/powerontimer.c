/**
   @file powerontimer.c

   This file implements part of the device poweron timer feature and
   provides the current value for interested sw components.
   <p>
   Copyright (C) 2010 Nokia Corporation
   Copyright (C) 2013-2017 Jolla Ltd.

   @author Simo Piiroinen <simo.piiroinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Jarkko Nikula <jarkko.nikula@jollamobile.com>
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
 * An example command line to obtain poweron time over D-Bus:
 * $ dbus-send --system --print-reply --dest=com.nokia.powerontimer /com/nokia/powerontimer com.nokia.powerontimer.get_poweron_time
 */

#include <iphbd/iphb_internal.h>

#include "powerontimer.h"

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/modules.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/logging.h"
#include "heartbeat.h"

#include <dsme/state.h>

#include <glib.h>
#include <stdlib.h>

#include "powerontimer_backend.h"

// prefix for log messages from this module
#define PFIX "poweron-timer: "

static bool      in_user_mode = false;

/** Flag for: dbus method call handlers have been installed */
static bool      dbus_methods_bound   = false;

/* ========================================================================= *
 * D-Bus Query API
 * ========================================================================= */

static void get_poweron_time(const DsmeDbusMessage* request,
                              DsmeDbusMessage**      reply)
{
  *reply = dsme_dbus_reply_new(request);
  dsme_dbus_message_append_int(*reply, (int)pot_get_poweron_secs());
}

static const char poweron_service[]   = "com.nokia.powerontimer";
static const char poweron_interface[] = "com.nokia.powerontimer";
static const char poweron_path[]      = "/com/nokia/powerontimer";

/** Array of power on timer method call handlers */
static const dsme_dbus_binding_t dbus_methods_array[] =
{
    // method calls
    {
        .method = get_poweron_time,
        .name   = "get_poweron_time",
        .args   =
            "    <arg direction=\"out\" name=\"seconds\" type=\"i\"/>\n"
    },
    // sentinel
    {
        .name   = 0,
    }
};

/* ========================================================================= *
 * CAL block updating
 * ========================================================================= */

static void poweron_update_cb(void)
{
  // update cal data without forcing write
  pot_update_cal(in_user_mode, false);

  // schedule the next update wakeup
  DSM_MSGTYPE_WAIT msg = DSME_MSG_INIT(DSM_MSGTYPE_WAIT);
  msg.req.mintime = 10;
  msg.req.maxtime = 60;
  msg.req.pid     = 0;
  msg.data        = 0;

  modules_broadcast_internally(&msg);
}

/* ========================================================================= *
 * Internal DSME event handling
 * ========================================================================= */

DSME_HANDLER(DSM_MSGTYPE_WAKEUP, client, msg)
{
  poweron_update_cb();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
  dsme_dbus_bind_methods(&dbus_methods_bound,
                         poweron_service,
                         poweron_path,
                         poweron_interface,
                         dbus_methods_array);
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
}

DSME_HANDLER(DSM_MSGTYPE_STATE_CHANGE_IND, server, msg)
{
  bool user_mode  = false;
  bool force_save = false;

  switch( msg->state )
  {
  case DSME_STATE_USER:
    user_mode = true;
    break;

  case DSME_STATE_SHUTDOWN:
  case DSME_STATE_REBOOT:
  case DSME_STATE_MALF:
    force_save = true;
    break;

  default:
    break;
  }

  pot_update_cal(user_mode, force_save);
  in_user_mode = user_mode;
}

module_fn_info_t message_handlers[] =
{
  DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_CHANGE_IND),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_WAKEUP),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
  { 0 }
};

/* ========================================================================= *
 * Plugin init and fini
 * ========================================================================= */

void module_init(module_t* handle)
{
  poweron_update_cb();
}

void module_fini(void)
{
  pot_update_cal(in_user_mode, true);

  dsme_dbus_unbind_methods(&dbus_methods_bound,
                           poweron_service,
                           poweron_path,
                           poweron_interface,
                           dbus_methods_array);
}
