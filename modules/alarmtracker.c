/**
 * @file alarmtracker.c
 *
 * Track the alarm state from the alarm queue indications sent by the time daemon (timed).
 * This is needed for device state selection in the state module;
 * if an alarm is set, we go to the acting dead state instead of a reboot or
 * shutdown. This allows the device to wake up due to an alarm.
 *
 * <p>
 * Copyright (C) 2009-2010 Nokia Corporation.
 * Copyright (C) 2013-2019 Jolla Ltd.
 *
 * @author Semi Malinen <semi.malinen@nokia.com>
 * @author Matias Muhonen <ext-matias.muhonen@nokia.com>
 * @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * This file is part of Dsme.
 *
 * Dsme is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * Dsme is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * How to send alarms to alarm tracker:
 * dbus-send --system --type=signal /com/nokia/time com.nokia.time.next_bootup_event int32:0
 * where the int32 parameter is either 0, meaning there are no pending alarms,
 * or the time of the next/current alarm as returned by time(2).
 * Notice that the time may be in the past for current alarms.
 */

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/timers.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "../dsme/utility.h"

#include <dsme/state.h>
#include <dsme/protocol.h>
#include <dsme/alarm_limit.h>

#include <iphbd/iphb_internal.h>

#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <string.h>

/* ========================================================================= *
 * Constants
 * ========================================================================= */
/** Prefix to use for all logging from this module */
#define PFIX "alarmtracker: "

/*
 * Store the alarm queue state in a file; it is used to restore the alarm queue state
 * when the module is loaded.
 */
#define ALARM_STATE_FILE     "/var/lib/dsme/alarm_queue_status"
#define ALARM_STATE_FILE_TMP "/var/lib/dsme/alarm_queue_status.tmp"

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static char *alarmtime_repr(time_t alarmtime, char *buff, size_t size);

/* ------------------------------------------------------------------------- *
 * ALARMTRACKER_ALARMTIME
 * ------------------------------------------------------------------------- */

static void   alarmtracker_alarmtime_schedule_save(void);
static void   alarmtracker_alarmtime_load         (void);
static void   alarmtracker_alarmtime_save         (void);
static void   alarmtracker_alarmtime_update       (time_t alarmtime);
static time_t alarmtracker_alarmtime_get          (void);
static int    alarmtracker_alarmtime_in           (void);

/* ------------------------------------------------------------------------- *
 * ALARMTRACKER_ALARMSTATE
 * ------------------------------------------------------------------------- */

static void alarmtracker_alarmstate_broadcast        (void);
static void alarmtracker_alarmstate_schedule_evaluate(int delay);
static void alarmtracker_alarmstate_cancel_evaluate  (void);
static int  alarmtracker_alarmstate_evaluate_cb      (void *aptr);
static void alarmtracker_alarmstate_evaluate         (void);

/* ------------------------------------------------------------------------- *
 * ALARMTRACKER_DSMESTATE
 * ------------------------------------------------------------------------- */

static dsme_state_t alarmtracker_dsmestate_get   (void);
static void         alarmtracker_dsmestate_update(dsme_state_t state);
static void         alarmtracker_dsmestate_query (void);

/* ------------------------------------------------------------------------- *
 * ALARMTRACKER_DBUS
 * ------------------------------------------------------------------------- */

static void alarmtracker_dbus_next_bootup_event_cb(const DsmeDbusMessage *ind);

/* ------------------------------------------------------------------------- *
 * MODULE
 * ------------------------------------------------------------------------- */

void module_init(module_t *handle);
void module_fini(void);

/* ========================================================================= *
 * Data
 * ========================================================================= */

/** The alarm queue status, as reported by timed
 *
 * 0 -> no alarms
 * 1 -> active alarm
 * n -> time of the next alarm
 */
static time_t alarmtracker_alarmtime_current = 0;

/** The alarm status stored in persistent cache file
 *
 * Used during bootup, while waiting for timed to re-evaluate and
 * broadcast actual queue state.
 */
static time_t alarmtracker_alarmtime_cached = 0;

/** DSME alarm state
 *
 * true  -> alarm is active / about to get triggered in near future
 * false -> no alarms queued / would trigger in distant future
 */
static bool alarmtracker_alarmstate_current = false;

/** Timer for re-evaluating alarm state
 *
 * For switching from: alarm in distant future -> alarm is about to trigger
 */
static dsme_timer_t alarmtracker_alarmstate_evaluate_id = 0;

/** Cached dsme state */
static dsme_state_t alarmtracker_dsmestate_current = DSME_STATE_NOT_SET;

/* ========================================================================= *
 * UTILITY
 * ========================================================================= */

/** Human readable alarm time representation for debugging purposes
 *
 * @param alarmtime alarm queue state as reported by timed
 *
 * @return human readable alarm time representation
 */
static char *
alarmtime_repr(time_t alarmtime, char *buff, size_t size)
{
    time_t now = time(0);

    if( alarmtime < 0 )
        snprintf(buff, size, "invalid");
    else if( alarmtime == 1 )
        snprintf(buff, size, "active-alarm");
    else if( alarmtime == 0 || alarmtime < now )
        snprintf(buff, size, "no-alarms");
    else
        snprintf(buff, size, "in-%d-secs", (int)(alarmtime - now));

    return buff;
}

/* ========================================================================= *
 * ALARMTRACKER_ALARMTIME
 * ========================================================================= */

/** Delayed update of persistently cached alarm time
 *
 * Uses iphb wakeup to handle the saving within the next two minutes.
 */
static void
alarmtracker_alarmtime_schedule_save(void)
{
    DSM_MSGTYPE_WAIT msg = DSME_MSG_INIT(DSM_MSGTYPE_WAIT);
    msg.req.mintime = 0;
    msg.req.maxtime = msg.req.mintime + 120;
    msg.req.pid     = 0;
    msg.data        = 0;

    dsme_log(LOG_DEBUG, PFIX"scheduled status save");
    modules_broadcast_internally(&msg);
}

/** Load persistently cached alarm time
 */
static void
alarmtracker_alarmtime_load(void)
{
    FILE *fh = 0;

    /* Reset value */
    alarmtracker_alarmtime_cached = 0;

    if( !(fh = fopen(ALARM_STATE_FILE, "r")) ) {
        /* Silently ignore non-existing cache file */
        if( errno != ENOENT )
            dsme_log(LOG_WARNING, PFIX"%s: can't open: %m",
                     ALARM_STATE_FILE);
        goto EXIT;
    }

    long data = 0;
    if( errno = 0, fscanf(fh, "%ld", &data) != 1 ) {
        dsme_log(LOG_DEBUG, PFIX"%s: read error: %m", ALARM_STATE_FILE);
        goto EXIT;
    }

    alarmtracker_alarmtime_cached  = (time_t)data;
    dsme_log(LOG_DEBUG, PFIX"Alarm queue head restored: %ld",
             alarmtracker_alarmtime_current);

EXIT:
    alarmtracker_alarmtime_update(alarmtracker_alarmtime_cached);

    if( fh )
        fclose(fh);
}

/** Update persistently cached alarm time
 */
static void
alarmtracker_alarmtime_save(void)
{
    FILE *fh = 0;

    dsme_log(LOG_DEBUG, PFIX"execute status save");

    if( alarmtracker_alarmtime_cached == alarmtracker_alarmtime_current ) {
        dsme_log(LOG_DEBUG, PFIX"%s is up to date",
                 ALARM_STATE_FILE);
        goto EXIT;
    }

    if( !(fh = fopen(ALARM_STATE_FILE_TMP, "w+")) ) {
        dsme_log(LOG_WARNING, PFIX"%s: can't open: %m",
                 ALARM_STATE_FILE_TMP);
        goto EXIT;
    }

    if( fprintf(fh, "%ld\n", alarmtracker_alarmtime_current) < 0) {
        dsme_log(LOG_WARNING, PFIX"%s: can't write: %m",
                 ALARM_STATE_FILE_TMP);
        goto EXIT;
    }

    if( fflush(fh) == EOF ) {
        dsme_log(LOG_WARNING, PFIX"%s: can't flush: %m",
                 ALARM_STATE_FILE_TMP);
        goto EXIT;
    }

    fclose(fh), fh = 0;

    if( rename(ALARM_STATE_FILE_TMP, ALARM_STATE_FILE) == -1 ) {
        dsme_log(LOG_WARNING, PFIX"%s: can't update: %m",
                 ALARM_STATE_FILE);
        goto EXIT;
    }

    dsme_log(LOG_DEBUG, PFIX"%s updated", ALARM_STATE_FILE);
    alarmtracker_alarmtime_cached = alarmtracker_alarmtime_current;

EXIT:
    if( fh )
        fclose(fh);

    return;
}

/** Handle alarm time changed notifications
 */
static void
alarmtracker_alarmtime_update(time_t alarmtime)
{
    if( alarmtracker_alarmtime_current != alarmtime ) {
        char prev[32], curr[32];
        dsme_log(LOG_DEBUG, PFIX"alarmtime: %s-> %s",
                 alarmtime_repr(alarmtracker_alarmtime_current,
                                prev, sizeof prev),
                 alarmtime_repr(alarmtime, curr, sizeof curr));

        alarmtracker_alarmtime_current = alarmtime;
        alarmtracker_alarmstate_evaluate();
    }

    if( alarmtracker_alarmtime_cached != alarmtracker_alarmtime_current ) {
        alarmtracker_alarmtime_schedule_save();
    }
}

/** Get currently known alarm time
 *
 * @return timestamp of the next alarm / 0 for no alarms / 1 for active alarm
 */
static time_t
alarmtracker_alarmtime_get(void)
{
    return alarmtracker_alarmtime_current;
}

/** Calculate delay until the next alarm time
 *
 * @returns seconds until the next alarm ought to trigger
 */
static int
alarmtracker_alarmtime_in(void)
{
    time_t trigger = alarmtracker_alarmtime_get();

    if( trigger <= 0 || trigger >= INT_MAX)
        return 9999;

    time_t now = time(0);

    return (trigger <= now) ? 0 : (int)(trigger - now);
}

/* ========================================================================= *
 * ALARMTRACKER_ALARMSTATE
 * ========================================================================= */

/** Broadcast alarm state changes within dsme, and to libdsme ipc clients
 */
static void
alarmtracker_alarmstate_broadcast(void)
{
    static bool prev = false;

    if( prev != alarmtracker_alarmstate_current ) {
        prev = alarmtracker_alarmstate_current;

        DSM_MSGTYPE_SET_ALARM_STATE msg =
            DSME_MSG_INIT(DSM_MSGTYPE_SET_ALARM_STATE);

        msg.alarm_set = alarmtracker_alarmstate_current;

        dsme_log(LOG_DEBUG, PFIX"broadcasting alarm state: %s",
                 alarmtracker_alarmstate_current ? "set" : "not set");

        /* inform state module about changed alarm state */
        modules_broadcast_internally(&msg);

        /* inform clients about the change in upcoming alarms */
        dsmesock_broadcast(&msg);
    }

    return;
}

/** Schedule delayed alarm state re-evaluation
 *
 * @param delay Seconds to wait
 */
static void
alarmtracker_alarmstate_schedule_evaluate(int delay)
{
    if( !alarmtracker_alarmstate_evaluate_id ) {
        /* TODO: does this work with suspend? */
        alarmtracker_alarmstate_evaluate_id =
            dsme_create_timer_seconds(delay,
                                      alarmtracker_alarmstate_evaluate_cb, 0);
        dsme_log(LOG_DEBUG, PFIX"evaluate again in %d s", delay);
    }
}

/** Cancel pending delayed alarm state re-evaluation
 */
static void
alarmtracker_alarmstate_cancel_evaluate(void)
{
    if( alarmtracker_alarmstate_evaluate_id ) {
        dsme_destroy_timer(alarmtracker_alarmstate_evaluate_id),
            alarmtracker_alarmstate_evaluate_id = 0;
        dsme_log(LOG_DEBUG, PFIX"re-evaluate canceled");
    }
}

/** Timer callback for delayed alarm state re-evaluation
 *
 * @param aptr  (Unused) user data
 *
 * @return 0 (to stop timer from repeating)
 */
static int
alarmtracker_alarmstate_evaluate_cb(void *aptr)
{
    (void)aptr;

    dsme_log(LOG_DEBUG, PFIX"re-evaluate triggered");

    alarmtracker_alarmstate_evaluate_id = 0;

    alarmtracker_alarmstate_evaluate();

    return 0; /* stop the interval */
}

/** Evaluate alarm state as applicable to dsme
 *
 * This function should be called whenever values used in evaluation
 * change.
 */
static void
alarmtracker_alarmstate_evaluate(void)
{
    bool   alarm_set  = false;
    time_t alarmtime  = alarmtracker_alarmtime_get();
    time_t alarm_in   = alarmtracker_alarmtime_in();
    time_t threshold  = dsme_snooze_timeout_in_seconds();

    /* stop pending re-evaluation timer */
    alarmtracker_alarmstate_cancel_evaluate();

    if( alarmtracker_dsmestate_get() == DSME_STATE_ACTDEAD &&
        dsme_home_is_encrypted() ) {
        /* encrypted home is not available in act-dead,
         * alarms can't be shown and must be ignored even
         * if timed would be reporting them. */
        alarm_set = false;
    }
    else if( alarmtime == 0 ) {
        /* there are no alarms */
        alarm_set = false;
    }
    else if( alarmtime == 1 ) {
        /* there is an active alarm */
        alarm_set = true;
    }
    else if( alarm_in <= threshold )
    {
        /* alarm is soon-to-be-active */
        alarm_set = true;
    } else {
        /* alarm is too far away */
        alarm_set = false;

        /* re-evaluate when the alarm ought to be about to trigger */
        alarmtracker_alarmstate_schedule_evaluate(alarm_in - threshold);
    }

    if( alarmtracker_alarmstate_current != alarm_set ) {
        dsme_log(LOG_DEBUG, PFIX"alarmstate: %d -> %d",
                 alarmtracker_alarmstate_current,
                 alarm_set);
        alarmtracker_alarmstate_current = alarm_set;
    }

    alarmtracker_alarmstate_broadcast();
}

/* ========================================================================= *
 * ALARMTRACKER_DSMESTATE
 * ========================================================================= */

/** Get cached dsme state
 *
 * @return return the last reported dsme state
 */
static dsme_state_t
alarmtracker_dsmestate_get(void)
{
  return alarmtracker_dsmestate_current;
}

/** Update cached dsme state
 *
 * @param state  dsme state from change notification
 */
static void
alarmtracker_dsmestate_update(dsme_state_t state)
{
    if( alarmtracker_dsmestate_current != state ) {
        dsme_log(LOG_DEBUG, PFIX"dsme_state: %s -> %s",
                 dsme_state_repr(alarmtracker_dsmestate_current),
                 dsme_state_repr(state));
        alarmtracker_dsmestate_current = state;
        alarmtracker_alarmstate_evaluate();
    }
}

/** Send internal to dsme query for dsme state
 *
 * This function should be called on module load to obtain initial state.
 */
static void
alarmtracker_dsmestate_query(void)
{
    /* get dsme state so that we can report it over D-Bus if asked to */
    DSM_MSGTYPE_STATE_QUERY req_state = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);
    modules_broadcast_internally(&req_state);
}

/* ========================================================================= *
 * ALARMTRACKER_DBUS
 * ========================================================================= */

/** Handle com.nokia.time.next_bootup_event D-Bus notification
 *
 * @param ind  dbus message in dsme wrapper
 */
static void
alarmtracker_dbus_next_bootup_event_cb(const DsmeDbusMessage *ind)
{
    time_t alarmtime = dsme_dbus_message_get_int(ind);

    alarmtracker_alarmtime_update(alarmtime);
}

/** Array of dbus signal handlers to install */
static const dsme_dbus_signal_binding_t dbus_signals_array[] =
{
    {
        .handler   = alarmtracker_dbus_next_bootup_event_cb,
        .interface = "com.nokia.time",
        .name      = "next_bootup_event"
    },
    {
        .handler = 0,
    }
};

/** Flag for: dbus signal handlers have been installed */
static bool dbus_signals_bound = false;

/* ========================================================================= *
 * DSME message handlers
 * ========================================================================= */

DSME_HANDLER(DSM_MSGTYPE_STATE_CHANGE_IND, server, msg)
{
    /* Change notification / reply to alarmtracker_dsmestate_query() */
    alarmtracker_dsmestate_update(msg->state);
}

DSME_HANDLER(DSM_MSGTYPE_WAKEUP, client, msg)
{
    /* Wakeup scheduled from alarmtracker_alarmtime_schedule_save() */
    alarmtracker_alarmtime_save();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DBUS_CONNECTED");
    dsme_dbus_bind_signals(&dbus_signals_bound, dbus_signals_array);
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DBUS_DISCONNECT");
}

DSME_HANDLER(DSM_MSGTYPE_STATE_QUERY, client, req)
{
    /* Query from some other plugin / via libdsme ipc */
    DSM_MSGTYPE_SET_ALARM_STATE resp =
        DSME_MSG_INIT(DSM_MSGTYPE_SET_ALARM_STATE);

    resp.alarm_set = alarmtracker_alarmstate_current;

    endpoint_send(client, &resp);
}

module_fn_info_t message_handlers[] =
{
    DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_CHANGE_IND),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_WAKEUP),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_QUERY),
    { 0 }
};

/* ========================================================================= *
 * MODULE load/unload
 * ========================================================================= */

/** Handle module load time actions
 */
void
module_init(module_t *handle)
{
    /* Do not connect to D-Bus; it is probably not started yet.
     * Instead, wait for DSM_MSGTYPE_DBUS_CONNECTED.
     */

    dsme_log(LOG_DEBUG, PFIX"loading plugin");

    alarmtracker_alarmtime_load();
    alarmtracker_dsmestate_query();
}

/** Handle module unload time actions
 */
void
module_fini(void)
{
    dsme_log(LOG_DEBUG, PFIX"unloading plugin");

    dsme_dbus_unbind_signals(&dbus_signals_bound, dbus_signals_array);
    alarmtracker_alarmtime_save();
    alarmtracker_alarmstate_cancel_evaluate();
}
