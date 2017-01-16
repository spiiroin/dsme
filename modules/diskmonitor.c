/**
   @file diskmonitor.c
   Periodically monitors the disks and sends a message if the disk space usage
   exceeds the use limits.

   <p>
   Copyright (C) 2011 Nokia Corporation.

   @author Matias Muhonen <ext-matias.muhonen@nokia.com>

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

// to send the init_done signal:
// dbus-send --system --type=signal /com/nokia/startup/signal com.nokia.startup.signal.init_done

// to request a disk space check:
// dbus-send --system --print-reply --dest=com.nokia.diskmonitor /com/nokia/diskmonitor/request com.nokia.diskmonitor.request.req_check

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#define LOGPFIX "diskmonitor: "

#include <iphbd/iphb_internal.h>

#include "dsme_dbus.h"
#include "dbusproxy.h"

#include "diskmonitor.h"
#include "diskmonitor_backend.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "heartbeat.h"

#include <sys/time.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool init_done_received           = false;
static bool device_active                = false;

static bool dbus_methods_bound           = false;
static bool dbus_signals_bound           = false;

static time_t last_check_time            = 0;

static const int ACTIVE_CHECK_INTERVAL   =   60;  /* When the device is used, this is how often we check the disk space */
static const int IDLE_CHECK_INTERVAL     = 1800;  /* When the device is not used, this is how often we check the disk space */
static const int MAXTIME_FROM_LAST_CHECK =   60;  /* After this many seconds from the last check, force a check when the device is actived */
static const int CHECK_THRESHOLD         =    5;  /* This is how often we allow disk space check to be requested */
static const int CHECK_MAX_LATENCY       =   12;  /* Should be >= heartbeat interval, which is 12 seconds */

/* ========================================================================= *
 * Helpers
 * ========================================================================= */

const char *diskspace_state_repr(diskspace_state_t state)
{
    const char *repr = "UNKNOWN";
    switch( state ) {
    case DISKSPACE_STATE_UNDEF:   repr = "UNDEF";   break;
    case DISKSPACE_STATE_NORMAL:  repr = "NORMAL";  break;
    case DISKSPACE_STATE_WARNING: repr = "WARNING"; break;
    default: break;
    }
    return repr;
}


static void monotime_get(struct timeval *tv)
{
    struct timespec ts = { 0, 0 };
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == -1) {
        dsme_log(LOG_WARNING, LOGPFIX"%s: %s", "CLOCK_BOOTTIME",
                 strerror(errno));
    }
    TIMESPEC_TO_TIMEVAL(tv, &ts);
}

static int disk_check_interval(void)
{
    int interval;

    if (!last_check_time) {
        interval = ACTIVE_CHECK_INTERVAL;
    }
    else if (device_active) {
        interval = ACTIVE_CHECK_INTERVAL;
    } else {
        interval = IDLE_CHECK_INTERVAL;
    }
    return interval;
}

static void schedule_next_wakeup(void)
{
    DSM_MSGTYPE_WAIT msg = DSME_MSG_INIT(DSM_MSGTYPE_WAIT);
    msg.req.mintime = disk_check_interval();
    msg.req.maxtime = msg.req.mintime + CHECK_MAX_LATENCY;
    msg.req.pid     = 0;
    msg.data        = 0;

    dsme_log(LOG_DEBUG, LOGPFIX"schedule next wakeup in: %d ... %d seconds",
             msg.req.mintime, msg.req.maxtime);

    broadcast_internally(&msg);
}

static void check_disk_space(void)
{
    struct timeval monotime;
    monotime_get(&monotime);

    if (init_done_received) {
        check_disk_space_usage(), last_check_time = monotime.tv_sec;
    }
}

/* ========================================================================= *
 * D-Bus Query API
 * ========================================================================= */

static const char diskmonitor_service[]               = "com.nokia.diskmonitor";
static const char diskmonitor_req_interface[]         = "com.nokia.diskmonitor.request";
static const char diskmonitor_sig_interface[]         = "com.nokia.diskmonitor.signal";
static const char diskmonitor_req_path[]              = "/com/nokia/diskmonitor/request";
static const char diskmonitor_sig_path[]              = "/com/nokia/diskmonitor/signal";

static const char diskmonitor_req_check[]             = "req_check";
static const char diskmonitor_disk_space_state_ind[]  = "disk_space_state_ind";

static void req_check(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
    struct timeval monotime;
    int seconds_from_last_check;

    monotime_get(&monotime);
    seconds_from_last_check = monotime.tv_sec - last_check_time;

    if (seconds_from_last_check >= CHECK_THRESHOLD) {
        check_disk_space();

        schedule_next_wakeup();
    } else {
        dsme_log(LOG_DEBUG,
                 LOGPFIX"%i seconds from the last disk space check request, skip this request",
                 seconds_from_last_check);
    }

    *reply = dsme_dbus_reply_new(request);
}

static const dsme_dbus_binding_t methods[] =
{
    { req_check, diskmonitor_req_check },
    { 0, 0 }
};

static void init_done_ind(const DsmeDbusMessage* ind)
{
    dsme_log(LOG_DEBUG, LOGPFIX"init_done received");

    init_done_received = true;
}

static void mce_inactivity_sig(const DsmeDbusMessage* sig)
{
    const bool inactive                 = dsme_dbus_message_get_bool(sig);
    const bool new_device_active_state  = !inactive;
    struct timeval monotime;
    int seconds_from_last_check;

    monotime_get(&monotime);
    seconds_from_last_check = monotime.tv_sec - last_check_time;

    dsme_log(LOG_DEBUG, LOGPFIX"device %s signal received",
             new_device_active_state ? "active" : "inactive");

    if (new_device_active_state == device_active) {
        /* no change in the inactivity state; don't adjust the schedule */
        return;
    }

    device_active = new_device_active_state;

    if(device_active) {
        /* If the last check has not been done recently enough,
         * check immediately on activation. */
        if (seconds_from_last_check >= MAXTIME_FROM_LAST_CHECK) {
            dsme_log(LOG_DEBUG, LOGPFIX"%d seconds from the last check",
                     seconds_from_last_check);
            check_disk_space();
        }

        /* Adjust the next wake-up to occur sooner */
        schedule_next_wakeup();
    }
    else {
        /* Leave the previously programmed timer running so that
         * we do one more check in device-is-active schedule. */
    }
}

static const dsme_dbus_signal_binding_t signals[] =
{
    { init_done_ind,      "com.nokia.startup.signal", "init_done" },
    { mce_inactivity_sig, "com.nokia.mce.signal",     "system_inactivity_ind" },
    { 0, 0 }
};

/* ========================================================================= *
 * Internal DSME event handling
 * ========================================================================= */

DSME_HANDLER(DSM_MSGTYPE_WAKEUP, client, msg)
{
    dsme_log(LOG_DEBUG, LOGPFIX"iphb timer wakeup");

    check_disk_space();

    schedule_next_wakeup();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, LOGPFIX"DBUS_CONNECT");

    dsme_dbus_bind_methods(&dbus_methods_bound, methods, diskmonitor_service, diskmonitor_req_interface);
    dsme_dbus_bind_signals(&dbus_signals_bound, signals);

    /* If dsme was restarted after bootup is finished, init-done signal
     * is not going to come again -> check flag file for initial state */
    if( access("/run/systemd/boot-status/init-done", F_OK) == 0 ) {
        dsme_log(LOG_DEBUG, LOGPFIX"init_done already passed");
        init_done_received = true;
        schedule_next_wakeup();
    }
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
   dsme_log(LOG_DEBUG, LOGPFIX"DBUS_DISCONNECT");

   dsme_dbus_unbind_methods(&dbus_methods_bound, methods, diskmonitor_service, diskmonitor_req_interface);
   dsme_dbus_unbind_signals(&dbus_signals_bound, signals);
}

DSME_HANDLER(DSM_MSGTYPE_DISK_SPACE, conn, msg)
{
    const char* mount_path = DSMEMSG_EXTRA(msg);

    dsme_log(LOG_DEBUG, LOGPFIX"send %s disk space notification for: %s",
             diskspace_state_repr(msg->diskspace_state), mount_path);

    DsmeDbusMessage* sig =
        dsme_dbus_signal_new(diskmonitor_sig_path,
                             diskmonitor_sig_interface,
                             diskmonitor_disk_space_state_ind);

    dsme_dbus_message_append_string(sig, mount_path);
    dsme_dbus_message_append_int(sig, msg->diskspace_state);
    dsme_dbus_signal_emit(sig);
}

module_fn_info_t message_handlers[] =
{
    DSME_HANDLER_BINDING(DSM_MSGTYPE_WAKEUP),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECT),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DISK_SPACE),
    { 0 }
};

/* ========================================================================= *
 * Plugin init and fini
 * ========================================================================= */

void module_init(module_t* module)
{
    dsme_log(LOG_DEBUG, "diskmonitor.so loaded");

    schedule_next_wakeup();
}

void module_fini(void)
{
    dsme_log(LOG_DEBUG, "diskmonitor.so unloaded");
}
