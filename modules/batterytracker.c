/**
   @file batterytracker.c

   Track the battery charge level. If charge level goes too low,
   issue warnings. If battery level goes below safe limit, make shutdown

   <p>
   Copyright (C) 2013-2017 Jolla Oy.

   @author Pekka Lundstrom <pekka.lundstrom@jolla.com>
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

#include "dbusproxy.h"
#include "dsme_dbus.h"

#include "../include/dsme/modulebase.h"
#include "../include/dsme/timers.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"

#include <dsme/state.h>
#include <dsme/protocol.h>

#include <iphbd/iphb_internal.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <string.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Logging prefix for this module */
#define PFIX "batterytracker: "

/** Path to optional battery level configuration file */
#define BATTERY_LEVEL_CONFIG_FILE  "/etc/dsme/battery_levels.conf"

/** Timer value for alarm shutdown timer.
 *
 * This is how long we wait before reporting battery empty when handling
 * alarms in act-dead mode - i.e. if user does not react to alarm, the
 * device will power off despite having an alarm on screen.
 */
#define ALARM_DELAYED_TIMEOUT 60

#define BATTERY_LEVEL_CRITICAL 1

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

/** String name <--> integer value mapping
 */
typedef struct
{
    const char *key;
    int         val;
} symbol_t;

/** Possible USB cable states
 */
typedef enum
{
    DSME_USB_CABLE_STATE_UNKNOWN,
    DSME_USB_CABLE_STATE_CONNECTED,
    DSME_USB_CABLE_STATE_DISCONNECTED,
} dsme_usb_cable_state_t;

/** Possible charger states
 */
typedef enum
{
    DSME_CHARGER_STATE_UNKNOWN,
    DSME_CHARGER_STATE_ON,
    DSME_CHARGER_STATE_OFF,
} dsme_charger_state_t;

/** Possible battery status values
 */
typedef enum
{
    DSME_BATTERY_STATUS_UNKNOWN,
    DSME_BATTERY_STATUS_FULL,
    DSME_BATTERY_STATUS_OK,
    DSME_BATTERY_STATUS_LOW,
    DSME_BATTERY_STATUS_EMPTY,
} dsme_battery_status_t;

/** DSME battery configuration levels
 */
typedef enum
{
    DSME_BATTERY_CONFIG_FULL,
    DSME_BATTERY_CONFIG_NORMAL,
    DSME_BATTERY_CONFIG_LOW,
    DSME_BATTERY_CONFIG_WARNING,
    DSME_BATTERY_CONFIG_EMPTY,

    DSME_BATTERY_CONFIG_COUNT
} battery_status_t;

/** DSME battery configuration level data
 */
typedef struct config_level_t
{
    int min_level;     /* percentance */
    int polling_time;  /* Polling time in sec */
    bool wakeup;       /* Resume from suspend to check battery level */
}  config_level_t;

/* ========================================================================= *
 * PROTOS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * Misc utils
 * ------------------------------------------------------------------------- */

static const char *bool_repr(bool val);

/* ------------------------------------------------------------------------- *
 * symbol
 * ------------------------------------------------------------------------- */

static int         symbol_lookup_val(const symbol_t *lut, const char *key, int def);
static const char *symbol_lookup_key(const symbol_t *lut, int val, const char *def);

/* ------------------------------------------------------------------------- *
 * dsme_usb_cable_state
 * ------------------------------------------------------------------------- */

static const char             *dsme_usb_cable_state_repr (dsme_usb_cable_state_t state);
static dsme_usb_cable_state_t  dsme_usb_cable_state_parse(const char *name);
static void                    dsme_usb_cable_state_set  (dsme_usb_cable_state_t state);

/* ------------------------------------------------------------------------- *
 * dsme_charger_state
 * ------------------------------------------------------------------------- */

static const char             *dsme_charger_state_repr   (dsme_charger_state_t state);
static dsme_charger_state_t    dsme_charger_state_parse  (const char *name);
static void                    dsme_charger_state_set    (dsme_charger_state_t state);

/* ------------------------------------------------------------------------- *
 * dsme_battery_status
 * ------------------------------------------------------------------------- */

static const char             *dsme_battery_status_repr  (dsme_battery_status_t state);
static dsme_battery_status_t   dsme_battery_status_parse (const char *name);
static void                    dsme_battery_status_set   (dsme_battery_status_t status);

/* ------------------------------------------------------------------------- *
 * dsme_battery_level
 * ------------------------------------------------------------------------- */

static const char             *dsme_battery_level_repr   (dsme_battery_level_t level);
static void                    dsme_battery_level_set    (dsme_battery_level_t level);

/* ------------------------------------------------------------------------- *
 * dsme_state
 * ------------------------------------------------------------------------- */

static const char             *dsme_state_repr           (dsme_state_t state);
static void                    dsme_state_set            (dsme_state_t state);

/* ------------------------------------------------------------------------- *
 * alarm_active
 * ------------------------------------------------------------------------- */

static void alarm_active_set    (bool active);

/* ------------------------------------------------------------------------- *
 * alarm_holdon
 * ------------------------------------------------------------------------- */

static int  alarm_holdon_cb    (void *unused);
static void alarm_holdon_start (void);
static void alarm_holdon_cancel(void);

/* ------------------------------------------------------------------------- *
 * config
 * ------------------------------------------------------------------------- */

static void config_load(void);

/* ------------------------------------------------------------------------- *
 * condition
 * ------------------------------------------------------------------------- */

static bool condition_battery_is_empty  (void);
static bool condition_charging_is_on    (void);
static bool condition_alarm_is_active   (void);
static bool condition_level_is_critical (void);

/* ------------------------------------------------------------------------- *
 * battery_empty_rethink
 * ------------------------------------------------------------------------- */

static int  battery_empty_rethink_cb      (void *aptr);
static void battery_empty_cancel_rethink  (void);
static void battery_empty_schedule_rethink(void);

/* ------------------------------------------------------------------------- *
 * xmce_running
 * ------------------------------------------------------------------------- */

static void              xmce_running_set                 (bool running);

/* ------------------------------------------------------------------------- *
 * xmce_tracking
 * ------------------------------------------------------------------------- */

static void              xmce_tracking_init               (void);
static void              xmce_tracking_quit               (void);

/* ------------------------------------------------------------------------- *
 * xmce_name_owner_query
 * ------------------------------------------------------------------------- */

static DBusHandlerResult xmce_name_owner_filter_cb        (DBusConnection *con, DBusMessage *msg, void *aptr);
static void              xmce_name_owner_reply_cb         (DBusPendingCall *pc, void *aptr);
static void              xmce_forget_mce_name_owner_query (void);
static void              xmce_send_name_owner_query       (void);

/* ------------------------------------------------------------------------- *
 * xmce_usb_cable_state_query
 * ------------------------------------------------------------------------- */

static void              xmce_usb_cable_state_signal_cb   (const DsmeDbusMessage *ind);
static void              xmce_usb_cable_state_reply_cb    (DBusPendingCall *pc, void *aptr);
static void              xmce_forget_usb_cable_state_query(void);
static void              xmce_send_usb_cable_state_query  (void);

/* ------------------------------------------------------------------------- *
 * xmce_charger_state_query
 * ------------------------------------------------------------------------- */

static void              xmce_charger_state_signal_cb     (const DsmeDbusMessage *ind);
static void              xmce_charger_state_reply_cb      (DBusPendingCall *pc, void *aptr);
static void              xmce_forget_charger_state_query  (void);
static void              xmce_send_charger_state_query    (void);

/* ------------------------------------------------------------------------- *
 * xmce_battery_status_query
 * ------------------------------------------------------------------------- */

static void              xmce_battery_status_signal_cb    (const DsmeDbusMessage *ind);
static void              xmce_battery_status_reply_cb     (DBusPendingCall *pc, void *aptr);
static void              xmce_forget_battery_status_query (void);
static void              xmce_send_battery_status_query   (void);

/* ------------------------------------------------------------------------- *
 * xmce_battery_level_query
 * ------------------------------------------------------------------------- */

static void              xmce_battery_level_signal_cb     (const DsmeDbusMessage *ind);
static void              xmce_battery_level_reply_cb      (DBusPendingCall *pc, void *aptr);
static void              xmce_forget_battery_level_query  (void);
static void              xmce_send_battery_level_query    (void);

/* ------------------------------------------------------------------------- *
 * systembus
 * ------------------------------------------------------------------------- */

static void systembus_connect   (void);
static void systembus_disconnect(void);

/* ------------------------------------------------------------------------- *
 * send
 * ------------------------------------------------------------------------- */

static void send_charger_state       (bool charging);
static void send_battery_state       (bool empty);
static void send_dsme_state_query    (void);

/* ------------------------------------------------------------------------- *
 * module
 * ------------------------------------------------------------------------- */

void module_init(module_t *handle);
void module_fini(void);

/* ========================================================================= *
 * State Data
 * ========================================================================= */

/** Cached module handle for this plugin */
static const module_t *this_module = 0;

/** Cached SystemBus connection */
static DBusConnection *systembus = 0;

/* ========================================================================= *
 * Misc Utils
 * ========================================================================= */

static const char *
bool_repr(bool val)
{
    return val ? "true" : "false";
}

/* ========================================================================= *
 * symbol
 * ========================================================================= */

static int
symbol_lookup_val(const symbol_t *lut, const char *key, int def)
{
    int val = def;

    for( ; lut->key; ++lut ) {
        if( !strcmp(lut->key, key) ) {
            val = lut->val;
            break;
        }
    }

    return val;
}

static const char *
symbol_lookup_key(const symbol_t *lut, int val, const char *def)
{
    const char *key = def;

    for( ; lut->key; ++lut ) {
        if( lut->val == val ) {
            key = lut->key;
            break;
        }
    }

    return key;
}

/* ========================================================================= *
 * dsme_usb_cable_state
 * ========================================================================= */

/** Lookup table for converting usb cable state names <--> enumeration values
 */
static const symbol_t dsme_usb_cable_state_lut[] =
{
    { MCE_USB_CABLE_STATE_UNKNOWN,      DSME_USB_CABLE_STATE_UNKNOWN      },
    { MCE_USB_CABLE_STATE_CONNECTED,    DSME_USB_CABLE_STATE_CONNECTED    },
    { MCE_USB_CABLE_STATE_DISCONNECTED, DSME_USB_CABLE_STATE_DISCONNECTED },
    { 0, }
};

static const char *
dsme_usb_cable_state_repr(dsme_usb_cable_state_t state)
{
    return symbol_lookup_key(dsme_usb_cable_state_lut, state, "invalid");
}

static dsme_usb_cable_state_t
dsme_usb_cable_state_parse(const char *name)
{
    return symbol_lookup_val(dsme_usb_cable_state_lut, name,
                             DSME_USB_CABLE_STATE_UNKNOWN);
}

static dsme_usb_cable_state_t dsme_usb_cable_state = DSME_USB_CABLE_STATE_UNKNOWN;

static void dsme_usb_cable_state_set(dsme_usb_cable_state_t state)
{
    if( dsme_usb_cable_state == state )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"dsme_usb_cable_state: %s -> %s",
             dsme_usb_cable_state_repr(dsme_usb_cable_state),
             dsme_usb_cable_state_repr(state));
    dsme_usb_cable_state = state;

    battery_empty_schedule_rethink();

EXIT:
    return;
}

/* ========================================================================= *
 * dsme_charger_state
 * ========================================================================= */

/** Lookup table for converting charger state names <--> enumeration values
 */
static const symbol_t dsme_charger_state_lut[] =
{

    { MCE_CHARGER_STATE_UNKNOWN, DSME_CHARGER_STATE_UNKNOWN },
    { MCE_CHARGER_STATE_ON,      DSME_CHARGER_STATE_ON      },
    { MCE_CHARGER_STATE_OFF,     DSME_CHARGER_STATE_OFF     },
    { 0, }
};

static const char *
dsme_charger_state_repr(dsme_charger_state_t state)
{
    return symbol_lookup_key(dsme_charger_state_lut, state, "invalid");
}

static dsme_charger_state_t
dsme_charger_state_parse(const char *name)
{
    return symbol_lookup_val(dsme_charger_state_lut, name,
                             DSME_CHARGER_STATE_UNKNOWN);
}

static dsme_charger_state_t dsme_charger_state = DSME_CHARGER_STATE_UNKNOWN;

static void dsme_charger_state_set(dsme_charger_state_t state)
{
    if( dsme_charger_state == state )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"dsme_charger_state: %s -> %s",
             dsme_charger_state_repr(dsme_charger_state),
             dsme_charger_state_repr(state));
    dsme_charger_state = state;

    battery_empty_schedule_rethink();

    if( dsme_charger_state != DSME_CHARGER_STATE_UNKNOWN ) {
        bool charging = (dsme_charger_state == DSME_CHARGER_STATE_ON);
        send_charger_state(charging);
    }

EXIT:
    return;
}

/* ========================================================================= *
 * dsme_battery_status
 * ========================================================================= */

/** Lookup table for converting battery status names <--> enumeration values
 */
static const symbol_t dsme_battery_status_lut[] =
{
    { MCE_BATTERY_STATUS_UNKNOWN, DSME_BATTERY_STATUS_UNKNOWN },
    { MCE_BATTERY_STATUS_FULL,    DSME_BATTERY_STATUS_FULL    },
    { MCE_BATTERY_STATUS_OK,      DSME_BATTERY_STATUS_OK      },
    { MCE_BATTERY_STATUS_LOW,     DSME_BATTERY_STATUS_LOW     },
    { MCE_BATTERY_STATUS_EMPTY,   DSME_BATTERY_STATUS_EMPTY   },
    { 0, }
};

static const char *
dsme_battery_status_repr(dsme_battery_status_t state)
{
    return symbol_lookup_key(dsme_battery_status_lut, state, "invalid");
}

static dsme_battery_status_t
dsme_battery_status_parse(const char *name)
{
    return symbol_lookup_val(dsme_battery_status_lut, name,
                             DSME_BATTERY_STATUS_UNKNOWN);
}

static dsme_battery_status_t dsme_battery_status = DSME_BATTERY_STATUS_UNKNOWN;

static void dsme_battery_status_set(dsme_battery_status_t status)
{
    if( dsme_battery_status == status )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"dsme_battery_status: %s -> %s",
             dsme_battery_status_repr(dsme_battery_status),
             dsme_battery_status_repr(status));
    dsme_battery_status = status;

    battery_empty_schedule_rethink();

EXIT:
    return;
}

/* ========================================================================= *
 * dsme_battery_level
 * ========================================================================= */

static const char *
dsme_battery_level_repr(dsme_battery_level_t level)
{
    if( level == DSME_BATTERY_LEVEL_UNKNOWN )
        return "unknown";

    if( level < DSME_BATTERY_LEVEL_MINIMUM ||
        level > DSME_BATTERY_LEVEL_MAXIMUM )
        return "invalid";

    /* Assume: two statically allocated buffers are enough to
     *         deal with "change from A to B" type diagnostic
     *         logging.
     */
    static char buf[2][8];
    static int  n = 0;

    int i = ++n & 1;
    snprintf(buf[i], sizeof *buf, "%d%%", (int)level);
    return buf[i];
}

static dsme_battery_level_t dsme_battery_level = DSME_BATTERY_LEVEL_UNKNOWN;

static void dsme_battery_level_set(dsme_battery_level_t level)
{
    if( dsme_battery_level == level )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"dsme_battery_level: %s -> %s",
             dsme_battery_level_repr(dsme_battery_level),
             dsme_battery_level_repr(level));
    dsme_battery_level = level;

    battery_empty_schedule_rethink();

EXIT:
    return;
}

/* ========================================================================= *
 * dsme_state
 * ========================================================================= */

static const char *
dsme_state_repr(dsme_state_t state)
{
    const char *repr = "UNKNOWN";

    switch( state ) {
    case DSME_STATE_SHUTDOWN:   repr = "SHUTDOWN"; break;
    case DSME_STATE_USER:       repr = "USER";     break;
    case DSME_STATE_ACTDEAD:    repr = "ACTDEAD";  break;
    case DSME_STATE_REBOOT:     repr = "REBOOT";   break;
    case DSME_STATE_BOOT:       repr = "BOOT";     break;
    case DSME_STATE_NOT_SET:    repr = "NOT_SET";  break;
    case DSME_STATE_TEST:       repr = "TEST";     break;
    case DSME_STATE_MALF:       repr = "MALF";     break;
    case DSME_STATE_LOCAL:      repr = "LOCAL";    break;
    default: break;
    }

    return repr;
}

static dsme_state_t dsme_state = DSME_STATE_NOT_SET;

static void
dsme_state_set(dsme_state_t state)
{
    if( dsme_state == state )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"dsme_state: %s -> %s",
             dsme_state_repr(dsme_state),
             dsme_state_repr(state));

    dsme_state = state;

    battery_empty_schedule_rethink();

EXIT:
    return;
}

/* ========================================================================= *
 * alarm_active
 * ========================================================================= */

static bool alarm_active = false;

static void
alarm_active_set(bool active)
{
    if( alarm_active == active )
        goto EXIT;

    dsme_log(LOG_INFO, PFIX"alarm_active: %s -> %s",
             bool_repr(alarm_active),
             bool_repr(active));

    alarm_active = active;

    if( alarm_active )
        alarm_holdon_start();
    else
        alarm_holdon_cancel();

    battery_empty_schedule_rethink();

EXIT:
    return;
}

/* ========================================================================= *
 * alarm_holdon
 * ========================================================================= */

static dsme_timer_t alarm_holdon_id = 0;

static int alarm_holdon_cb(void* unused)
{
    dsme_log(LOG_INFO, PFIX"Alarm hold on time is over");
    alarm_holdon_id = 0;

    /* Simulate end-of-alarm */
    alarm_active_set(false);

    return 0; /* stop the interval */
}

static void
alarm_holdon_start(void)
{
    if( !alarm_holdon_id ) {
        dsme_log(LOG_INFO, PFIX"Alarm hold on time started");
        alarm_holdon_id =
            dsme_create_timer_seconds(ALARM_DELAYED_TIMEOUT,
                                      alarm_holdon_cb, NULL);
    }
}

static void
alarm_holdon_cancel(void)
{
    if( alarm_holdon_id ) {
        dsme_log(LOG_INFO, PFIX"Alarm hold on time canceled");
        dsme_destroy_timer(alarm_holdon_id),
            alarm_holdon_id = 0;
    }
}

/* ========================================================================= *
 * config
 * ========================================================================= */

/** Lookup table for battery configuration level names
 */
static const char * const config_level_name[DSME_BATTERY_CONFIG_COUNT] =
{
    [DSME_BATTERY_CONFIG_FULL]    = "FULL",
    [DSME_BATTERY_CONFIG_NORMAL]  = "NORMAL",
    [DSME_BATTERY_CONFIG_LOW]     = "LOW",
    [DSME_BATTERY_CONFIG_WARNING] = "WARNING",
    [DSME_BATTERY_CONFIG_EMPTY]   = "EMPTY",
};

/** This is default config for battery levels
 *
 * It can be overridden by external file BATTERY_LEVEL_CONFIG_FILE.
 */
static config_level_t config_level_data[DSME_BATTERY_CONFIG_COUNT] =
{
    /* Min %, polling time */
    {  80, 300, false  }, /* Full    80 - 100, polling 5 mins */
    {  20, 180, false  }, /* Normal  20 - 79 */
    {  10, 120, true   }, /* Low     10 - 19 */
    {   3,  60, true   }, /* Warning  3 -  9, shutdown happens below this */
    {   0,  60, true   }  /* Empty    0 -  2, shutdown should have happened already  */
};

/** Load battery level configuration file
 *
 * If external config file exists, then read it and use
 * values defined there for battery level and polling times.
 *
 * @note: Since the battery data is no longer polled, the
 *        only relevant data in the config is the defined
 *        battery empty shutdown limit.
 */
static void config_load(void)
{
    FILE *input   = 0;
    bool  success = false;

    config_level_t temp[DSME_BATTERY_CONFIG_COUNT] = {};

    if( !(input = fopen(BATTERY_LEVEL_CONFIG_FILE, "r")) ) {
        if( errno != ENOENT )
            dsme_log(LOG_ERR, PFIX"%s: can't read config: %m",
                     BATTERY_LEVEL_CONFIG_FILE);
        goto EXIT;
    }

    for( size_t i = 0; i < DSME_BATTERY_CONFIG_COUNT; ++i ) {
        int wakeup = 0;
        int values = fscanf(input, "%d, %d, %d",
                            &temp[i].min_level,
                            &temp[i].polling_time,
                            &wakeup);

        /* Must define at least "min_level" and "polling time".
         */
        if( values < 2 ) {
            dsme_log(LOG_ERR, PFIX"%s:%d: %s: not enough data",
                     BATTERY_LEVEL_CONFIG_FILE, i+1,
                     config_level_name[i]);
            goto EXIT;
        }

        /* The "wakeup" is optional, default is to wakeup from
         * suspend to check battery level when on LOW/WARNING/EMPTY.
         */
        if( values < 3 )
            temp[i].wakeup = (i >= DSME_BATTERY_CONFIG_LOW);
        else
            temp[i].wakeup = (wakeup != 0);

        /* Do some sanity checking for values
         *
         * Battery level values must be in 0-100 range, and in descending order.
         *
         * Polling times should also make sense  10-1000s
         */
        if( temp[i].polling_time < 10 || temp[i].polling_time > 1000 ) {
            dsme_log(LOG_ERR, PFIX"%s:%d: %s: invalid polling_time=%d",
                     BATTERY_LEVEL_CONFIG_FILE, i+1,
                     config_level_name[i],
                     temp[i].polling_time);
            goto EXIT;
        }

        if( temp[i].min_level < 0 || temp[i].min_level > 100 ) {
            dsme_log(LOG_ERR, PFIX"%s:%d: %s: invalid min_level=%d",
                     BATTERY_LEVEL_CONFIG_FILE, i+1,
                     config_level_name[i],
                     temp[i].min_level);
            goto EXIT;
        }

        if( (i > 0) && temp[i-1].min_level <= temp[i].min_level ) {
            dsme_log(LOG_ERR, PFIX"%s:%d: %s: min_level=%d is not descending",
                     BATTERY_LEVEL_CONFIG_FILE, i+1,
                     config_level_name[i],
                     temp[i].min_level);
            goto EXIT;
        }
    }

    success = true;

EXIT:
    if( input )
        fclose(input);

    if( success ) {
        memcpy(config_level_data, temp, sizeof config_level_data);
        dsme_log(LOG_INFO, PFIX"Using battery level values from %s",
                 BATTERY_LEVEL_CONFIG_FILE);
    }
    else {
        dsme_log(LOG_DEBUG, PFIX"Using internal battery level values");
    }

    dsme_log(LOG_DEBUG, PFIX"Shutdown limit is < %d%%",
             config_level_data[DSME_BATTERY_CONFIG_WARNING].min_level);
}

/* ========================================================================= *
 * condition
 * ========================================================================= */

/** Predicate for: Battery level is known to be empty
 */
static bool condition_battery_is_empty(void)
{
    bool empty = true;

    int limit = config_level_data[DSME_BATTERY_CONFIG_WARNING].min_level;

    if( dsme_battery_level == DSME_BATTERY_LEVEL_UNKNOWN ) {
        /* Do not equate unknown with empty */
        empty = false;
    }
    else if( dsme_battery_level < DSME_BATTERY_LEVEL_MINIMUM ||
             dsme_battery_level > DSME_BATTERY_LEVEL_MAXIMUM ) {
        /* Do not equate invalid with empty */
        empty = false;
    }
    else if( dsme_battery_level >= limit ) {
        /* Known to be above the limit */
        empty = false;
    }

    return empty;
}

/** Predicate for: Battery is getting charged
 */
static bool condition_charging_is_on(void)
{
    return dsme_charger_state == DSME_CHARGER_STATE_ON;
}

/** Predicate for: UI is handling active alarm
 */
static bool condition_alarm_is_active(void)
{
    return alarm_active;
}

/** Predicate for: Battery level is dropping despite charging
 */
static bool condition_level_is_critical(void)
{
    return (dsme_state != DSME_STATE_ACTDEAD &&
            dsme_battery_level <= BATTERY_LEVEL_CRITICAL);
}

/* ========================================================================= *
 * battery_empty_rethink
 * ========================================================================= */

/** Idle timer for: Evaluate battery empty shutdown condition */
static dsme_timer_t battery_empty_rethink_id = 0;

/** Timer callback for: Evaluate battery empty shutdown condition */
static int battery_empty_rethink_cb(void *aptr)
{
    (void)aptr;

    battery_empty_rethink_id = 0;

    static bool shutdown_requested = false;

    bool request_shutdown = false;

    if( condition_battery_is_empty() ) {
        request_shutdown = true;

        /* Do not shutdown while handling active alarm
         */
        if( request_shutdown && condition_alarm_is_active() ) {
            request_shutdown = false;
            dsme_log(LOG_DEBUG, PFIX"Active alarm - do not shutdown");
        }

        /* Before starting shutdown, check charging. If charging is
         * going, give battery change to charge. But if battery level
         * keeps dropping even charger is connected (could be that we get only 100 mA)
         * we need to do shutdown. But let charging continue in actdead state
         */
        if( request_shutdown && condition_charging_is_on() ) {
            request_shutdown = false;
            dsme_log(LOG_DEBUG, PFIX"Charging - do not shutdown");
        }

        /* If charging in USER state, make sure level won't drop too much, keep min 1% */
        if( !request_shutdown && condition_level_is_critical() ) {
            request_shutdown = true;
            dsme_log(LOG_INFO, PFIX"Battery level keeps dropping - must shutdown");
        }
    }

    if( shutdown_requested != request_shutdown ) {
        dsme_log(LOG_CRIT, PFIX"Battery empty shutdown %s",
                 request_shutdown ? "requested" : "canceled");

        shutdown_requested = request_shutdown;
        send_battery_state(shutdown_requested);
    }

    return 0; /* stop the interval */
}

/** Cancel already scheduled battery empty shutdown evaluation
 */
static void
battery_empty_cancel_rethink(void)
{
    if( battery_empty_rethink_id ) {
        dsme_destroy_timer(battery_empty_rethink_id),
            battery_empty_rethink_id = 0;
    }
}

/** Schedule evaluation of battery empty shutdown condition
 */
static void
battery_empty_schedule_rethink(void)
{
    if( !battery_empty_rethink_id ) {
        battery_empty_rethink_id =
            dsme_create_timer_seconds(0, battery_empty_rethink_cb, 0);
    }
}

/* ========================================================================= *
 * xmce_running
 * ========================================================================= */

/** Flag for: MCE D-Bus service is available on SystemBus */
static bool xmce_running = false;

/** Change availability of MCE on SystemBus status
 *
 * @param running whether MCE_SERVICE has an owner or not
 */
static void
xmce_running_set(bool running)
{
    dsme_log(LOG_DEBUG, PFIX"xmce_running=%d running=%d",
             xmce_running, running);

    if( xmce_running == running )
        goto cleanup;

    xmce_running = running;

    dsme_log(LOG_DEBUG, PFIX"mce is %s",  xmce_running ? "running" : "stopped");

    if( xmce_running ) {
        xmce_send_usb_cable_state_query();
        xmce_send_charger_state_query();
        xmce_send_battery_status_query();
        xmce_send_battery_level_query();
    }
    else {
        xmce_forget_usb_cable_state_query();
        xmce_forget_charger_state_query();
        xmce_forget_battery_status_query();
        xmce_forget_battery_level_query();
    }

cleanup:

    return;
}

/* ========================================================================= *
 * xmce_tracking
 * ========================================================================= */

/** Signal matching rule for MCE name ownership changes */
static const char xmce_name_owner_match[] =
"type='signal'"
",sender='"DBUS_SERVICE_DBUS"'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='NameOwnerChanged'"
",path='"DBUS_PATH_DBUS"'"
",arg0='"MCE_SERVICE"'"
;

/** Start tracking MCE state on SystemBus
 */
static void
xmce_tracking_init(void)
{
    if( !systembus )
        goto cleanup;

    /* Register signal handling filter */
    dbus_connection_add_filter(systembus, xmce_name_owner_filter_cb, 0, 0);

    /* NULL error -> match will be added asynchronously */
    dbus_bus_add_match(systembus, xmce_name_owner_match, 0);

    /* Find out if MCE is running - and then continue with
     * state queries */
    xmce_send_name_owner_query();

cleanup:

    return;
}

/** Stop tracking MCE state on SystemBus
 */
static void
xmce_tracking_quit(void)
{
    if( !systembus )
        goto cleanup;

    /* Remove signal handling filter */
    dbus_connection_remove_filter(systembus, xmce_name_owner_filter_cb, 0);

    /* NULL error -> match will be removed asynchronously */
    dbus_bus_remove_match(systembus, xmce_name_owner_match, 0);

    /* Do not leave pending calls behind */
    xmce_forget_mce_name_owner_query();
    xmce_forget_usb_cable_state_query();
    xmce_forget_charger_state_query();
    xmce_forget_battery_status_query();
    xmce_forget_battery_level_query();

cleanup:

    return;
}

/* ========================================================================= *
 * xmce_name_owner_query
 * ========================================================================= */

/** Pending mce name owner query */
static DBusPendingCall *xmce_name_owner_query_pc = 0;

/** D-Bus message filter for handling MCE NameOwnerChanged signals
 *
 * @param con       dbus connection
 * @param msg       message to be acted upon
 * @param aptr      user data (unused)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static DBusHandlerResult
xmce_name_owner_filter_cb(DBusConnection *con, DBusMessage *msg, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *sender = 0;
    const char *object = 0;

    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    DBusError err = DBUS_ERROR_INIT;

    if( con != systembus )
        goto cleanup;

    if( !dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged") )
        goto cleanup;

    sender = dbus_message_get_sender(msg);
    if( strcmp(sender, DBUS_SERVICE_DBUS) )
        goto cleanup;

    object = dbus_message_get_path(msg);
    if( strcmp(object, DBUS_PATH_DBUS) )
        goto cleanup;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) ) {
        dsme_log(LOG_WARNING, PFIX"name owner signal: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    if( !strcmp(name, MCE_SERVICE) ) {
        dsme_log(LOG_DEBUG, PFIX"mce name owner: %s", curr);
        xmce_running_set(*curr != 0);
    }

cleanup:

    dbus_error_free(&err);

    enter_module(caller);
    return res;
}

/** D-Bus callback for handling to MCE GetNameOwner reply messages
 *
 * @param pending   Control structure for asynchronous d-bus methdod call
 * @param aptr      User data pointer (unused)
 */
static void
xmce_name_owner_reply_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusMessage *rsp = 0;
    const char  *dta = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( xmce_name_owner_query_pc != pc )
        goto cleanup;

    dbus_pending_call_unref(xmce_name_owner_query_pc),
        xmce_name_owner_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
            dsme_log(LOG_WARNING, PFIX"mce name owner error reply: %s: %s",
                     err.name, err.message);
        }
    }
    else if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &dta,
                               DBUS_TYPE_INVALID) ) {
        dsme_log(LOG_WARNING, PFIX"mce name owner parse error: %s: %s",
                 err.name, err.message);
    }
    else {
        dsme_log(LOG_DEBUG, PFIX"mce name owner reply: %s", dta);
    }

    xmce_running_set(dta && *dta);

cleanup:

    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);
    enter_module(caller);
}

/** Cancel pending mce name owner query
 */
static void
xmce_forget_mce_name_owner_query(void)
{
    if( !xmce_name_owner_query_pc )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX"forget mce name owner query");
    dbus_pending_call_cancel(xmce_name_owner_query_pc);
    dbus_pending_call_unref(xmce_name_owner_query_pc),
        xmce_name_owner_query_pc = 0;

EXIT:
    return;
}

/** Check availability of MCE D-Bus service via a GetNameOwner method call
 *
 * @return true if the method call was initiated, or false in case of errors
 */
static void
xmce_send_name_owner_query(void)
{
    dsme_log(LOG_DEBUG, PFIX"mce name owner query");

    bool             res  = false;
    DBusMessage     *req  = 0;
    DBusPendingCall *pc   = 0;
    const char      *name = MCE_SERVICE;

    if( !systembus )
        goto cleanup;

    xmce_forget_mce_name_owner_query();

    req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "GetNameOwner");
    if( !req )
        goto cleanup;

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_INVALID) )
        goto cleanup;

    if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
        goto cleanup;

    if( !pc )
        goto cleanup;

    if( !dbus_pending_call_set_notify(pc, xmce_name_owner_reply_cb, 0, 0) )
        goto cleanup;

    xmce_name_owner_query_pc = pc, pc = 0;

    res = true;

cleanup:

    if( res )
        dsme_log(LOG_DEBUG, PFIX"mce name owner query sent");
    else
        dsme_log(LOG_ERR, PFIX"failed to send mce name owner query");

    if( pc )
        dbus_pending_call_unref(pc);
    if( req )
        dbus_message_unref(req);
}

/* ========================================================================= *
 * xmce_usb_cable_state_query
 * ========================================================================= */

/** Pending usb cable state query */
static DBusPendingCall *xmce_usb_cable_state_query_pc = 0;

/** Handle reply to async usb cable state query
 */
static void
xmce_usb_cable_state_reply_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusMessage *rsp = 0;
    const char  *arg = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( xmce_usb_cable_state_query_pc != pc )
        goto cleanup;

    dbus_pending_call_unref(xmce_usb_cable_state_query_pc),
        xmce_usb_cable_state_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        dsme_log(LOG_ERR, PFIX"cable_state error reply: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &arg,
                               DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_ERR, PFIX"cable_state parse error: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    dsme_log(LOG_DEBUG, PFIX"cable_state reply: %s", arg);
    dsme_usb_cable_state_t state = dsme_usb_cable_state_parse(arg);
    dsme_usb_cable_state_set(state);

cleanup:

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);
    enter_module(caller);
}

/** Cancel pending usb cable state query
 */
static void
xmce_forget_usb_cable_state_query(void)
{
    if( !xmce_usb_cable_state_query_pc )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX"forget cable_state query");
    dbus_pending_call_cancel(xmce_usb_cable_state_query_pc);
    dbus_pending_call_unref(xmce_usb_cable_state_query_pc),
        xmce_usb_cable_state_query_pc = 0;

EXIT:
    return;
}

/** Initiate async usb cable state query
 */
static void
xmce_send_usb_cable_state_query(void)
{
    bool             res  = false;
    DBusPendingCall *pc   = 0;
    DBusMessage     *req  = NULL;

    if( !systembus )
        goto cleanup;

    xmce_forget_usb_cable_state_query();

    req = dbus_message_new_method_call(MCE_SERVICE,
                                       MCE_REQUEST_PATH,
                                       MCE_REQUEST_IF,
                                       MCE_USB_CABLE_STATE_GET);
    if( !req )
        goto cleanup;

    if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
        goto cleanup;

    if( !pc )
        goto cleanup;

    if( !dbus_pending_call_set_notify(pc, xmce_usb_cable_state_reply_cb, 0, 0) )
        goto cleanup;

    xmce_usb_cable_state_query_pc = pc, pc = 0;

    res = true;

cleanup:
    if( res )
        dsme_log(LOG_DEBUG, PFIX"cable_state query sent");
    else
        dsme_log(LOG_ERR, PFIX"failed to send cable_state query");

    if( pc )
        dbus_pending_call_unref(pc);
    if( req )
        dbus_message_unref(req);
}

/* ========================================================================= *
 * xmce_charger_state_query
 * ========================================================================= */

/** Pending charger state query */
static DBusPendingCall *xmce_charger_state_query_pc = 0;

/** Handle reply to async charger state query
 */
static void
xmce_charger_state_reply_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusMessage *rsp = 0;
    const char  *arg = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( xmce_charger_state_query_pc != pc )
        goto cleanup;

    dbus_pending_call_unref(xmce_charger_state_query_pc),
        xmce_charger_state_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        dsme_log(LOG_ERR, PFIX"charger_state error reply: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &arg,
                               DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_ERR, PFIX"charger_state parse error: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    dsme_log(LOG_DEBUG, PFIX"charger_state reply: %s", arg);
    dsme_charger_state_t state = dsme_charger_state_parse(arg);
    dsme_charger_state_set(state);

cleanup:

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);
    enter_module(caller);
}

/** Cancel pending charger state query
 */
static void
xmce_forget_charger_state_query(void)
{
    if( !xmce_charger_state_query_pc )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX"forget charger_state query");
    dbus_pending_call_cancel(xmce_charger_state_query_pc);
    dbus_pending_call_unref(xmce_charger_state_query_pc),
        xmce_charger_state_query_pc = 0;

EXIT:
    return;
}

/** Initiate async charger state query
 */
static void
xmce_send_charger_state_query(void)
{
    bool             res  = false;
    DBusPendingCall *pc   = 0;
    DBusMessage     *req  = NULL;

    if( !systembus )
        goto cleanup;

    xmce_forget_charger_state_query();

    req = dbus_message_new_method_call(MCE_SERVICE,
                                       MCE_REQUEST_PATH,
                                       MCE_REQUEST_IF,
                                       MCE_CHARGER_STATE_GET);
    if( !req )
        goto cleanup;

    if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
        goto cleanup;

    if( !pc )
        goto cleanup;

    if( !dbus_pending_call_set_notify(pc, xmce_charger_state_reply_cb, 0, 0) )
        goto cleanup;

    xmce_charger_state_query_pc = pc, pc = 0;

    res = true;

cleanup:
    if( res )
        dsme_log(LOG_DEBUG, PFIX"charger_state query sent");
    else
        dsme_log(LOG_ERR, PFIX"failed to send charger_state query");

    if( pc )
        dbus_pending_call_unref(pc);
    if( req )
        dbus_message_unref(req);
}

/* ========================================================================= *
 * xmce_battery_status_query
 * ========================================================================= */

/** Pending battery status query */
static DBusPendingCall *xmce_battery_status_query_pc = 0;

/** Handle reply to async battery status query
 */
static void
xmce_battery_status_reply_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusMessage *rsp = 0;
    const char  *arg = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( xmce_battery_status_query_pc != pc )
        goto cleanup;

    dbus_pending_call_unref(xmce_battery_status_query_pc),
        xmce_battery_status_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        dsme_log(LOG_ERR, PFIX"battery_status error reply: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &arg,
                               DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_ERR, PFIX"battery_status parse error: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    dsme_log(LOG_DEBUG, PFIX"battery_status reply: %s", arg);
    dsme_battery_status_t status = dsme_battery_status_parse(arg);
    dsme_battery_status_set(status);

cleanup:

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);
    enter_module(caller);
}

/** Cancel pending battery status query
 */
static void
xmce_forget_battery_status_query(void)
{
    if( !xmce_battery_status_query_pc )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX"forget battery_status query");
    dbus_pending_call_cancel(xmce_battery_status_query_pc);
    dbus_pending_call_unref(xmce_battery_status_query_pc),
        xmce_battery_status_query_pc = 0;

EXIT:
    return;
}

/** Initiate async battery status query
 */
static void
xmce_send_battery_status_query(void)
{
    bool             res  = false;
    DBusPendingCall *pc   = 0;
    DBusMessage     *req  = NULL;

    if( !systembus )
        goto cleanup;

    xmce_forget_battery_status_query();

    req = dbus_message_new_method_call(MCE_SERVICE,
                                       MCE_REQUEST_PATH,
                                       MCE_REQUEST_IF,
                                       MCE_BATTERY_STATUS_GET);
    if( !req )
        goto cleanup;

    if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
        goto cleanup;

    if( !pc )
        goto cleanup;

    if( !dbus_pending_call_set_notify(pc, xmce_battery_status_reply_cb, 0, 0) )
        goto cleanup;

    xmce_battery_status_query_pc = pc, pc = 0;

    res = true;

cleanup:
    if( res )
        dsme_log(LOG_DEBUG, PFIX"battery_status query sent");
    else
        dsme_log(LOG_ERR, PFIX"failed to send battery_status query");

    if( pc )
        dbus_pending_call_unref(pc);
    if( req )
        dbus_message_unref(req);
}

/* ========================================================================= *
 * xmce_battery_level_query
 * ========================================================================= */

/** Pending battery level query */
static DBusPendingCall *xmce_battery_level_query_pc = 0;

/** Handle reply to async battery level query
 */
static void
xmce_battery_level_reply_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr; // not used

    const module_t *caller = enter_module(this_module);

    DBusMessage *rsp = 0;
    dbus_int32_t arg = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( xmce_battery_level_query_pc != pc )
        goto cleanup;

    dbus_pending_call_unref(xmce_battery_level_query_pc),
        xmce_battery_level_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        dsme_log(LOG_ERR, PFIX"battery_level error reply: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_INT32, &arg,
                               DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_ERR, PFIX"battery_level parse error: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    dsme_log(LOG_DEBUG, PFIX"battery_level reply: %d", (int)arg);
    dsme_battery_level_t level = arg;
    dsme_battery_level_set(level);

cleanup:

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);
    enter_module(caller);
}

/** Cancel pending battery level query
 */
static void
xmce_forget_battery_level_query(void)
{
    if( !xmce_battery_level_query_pc )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX"forget battery_level query");
    dbus_pending_call_cancel(xmce_battery_level_query_pc);
    dbus_pending_call_unref(xmce_battery_level_query_pc),
        xmce_battery_level_query_pc = 0;

EXIT:
    return;
}

/** Initiate async battery level query
 */
static void
xmce_send_battery_level_query(void)
{
    bool             res  = false;
    DBusPendingCall *pc   = 0;
    DBusMessage     *req  = NULL;

    if( !systembus )
        goto cleanup;

    xmce_forget_battery_level_query();

    req = dbus_message_new_method_call(MCE_SERVICE,
                                       MCE_REQUEST_PATH,
                                       MCE_REQUEST_IF,
                                       MCE_BATTERY_LEVEL_GET);
    if( !req )
        goto cleanup;

    if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
        goto cleanup;

    if( !pc )
        goto cleanup;

    if( !dbus_pending_call_set_notify(pc, xmce_battery_level_reply_cb, 0, 0) )
        goto cleanup;

    xmce_battery_level_query_pc = pc, pc = 0;

    res = true;

cleanup:
    if( res )
        dsme_log(LOG_DEBUG, PFIX"battery_level query sent");
    else
        dsme_log(LOG_ERR, PFIX"failed to send battery_level query");

    if( pc )
        dbus_pending_call_unref(pc);
    if( req )
        dbus_message_unref(req);
}

/* ========================================================================= *
 * Incoming D-Bus signal messages
 * ========================================================================= */

/** Handler for MCE_USB_CABLE_STATE_SIG D-Bus signals
 */
static void
xmce_usb_cable_state_signal_cb(const DsmeDbusMessage* ind)
{
    const char *arg = dsme_dbus_message_get_string(ind);

    dsme_log(LOG_DEBUG, PFIX"dbus signal: %s(%s)",
             MCE_USB_CABLE_STATE_SIG, arg);

    dsme_usb_cable_state_t state = dsme_usb_cable_state_parse(arg);
    dsme_usb_cable_state_set(state);
}

/** Handler for MCE_CHARGER_STATE_SIG D-Bus signals
 */
static void
xmce_charger_state_signal_cb(const DsmeDbusMessage* ind)
{
    const char *arg = dsme_dbus_message_get_string(ind);

    dsme_log(LOG_DEBUG, PFIX"dbus signal: %s(%s)",
             MCE_CHARGER_STATE_SIG, arg);

    dsme_charger_state_t state = dsme_charger_state_parse(arg);
    dsme_charger_state_set(state);
}

/** Handler for MCE_BATTERY_STATUS_SIG D-Bus signals
 */
static void
xmce_battery_status_signal_cb(const DsmeDbusMessage* ind)
{
    const char *arg = dsme_dbus_message_get_string(ind);

    dsme_log(LOG_DEBUG, PFIX"dbus signal: %s(%s)",
             MCE_BATTERY_STATUS_SIG, arg);

    dsme_battery_status_t status = dsme_battery_status_parse(arg);
    dsme_battery_status_set(status);
}

/** Handler for MCE_BATTERY_LEVEL_SIG D-Bus signals
 */
static void
xmce_battery_level_signal_cb(const DsmeDbusMessage* ind)
{
    int arg = dsme_dbus_message_get_int(ind);

    dsme_log(LOG_DEBUG, PFIX"dbus signal: %s(%d)",
             MCE_BATTERY_STATUS_SIG, arg);

    dsme_battery_level_t level = arg;
    dsme_battery_level_set(level);
}

/** Array of D-Bus signals to listen */
static const dsme_dbus_signal_binding_t dbus_signals_array[] =
{
    { xmce_usb_cable_state_signal_cb, MCE_SIGNAL_IF,  MCE_USB_CABLE_STATE_SIG },
    { xmce_charger_state_signal_cb,   MCE_SIGNAL_IF,  MCE_CHARGER_STATE_SIG   },
    { xmce_battery_status_signal_cb,  MCE_SIGNAL_IF,  MCE_BATTERY_STATUS_SIG  },
    { xmce_battery_level_signal_cb,   MCE_SIGNAL_IF,  MCE_BATTERY_LEVEL_SIG   },
    { 0, 0 }
};

/** Flag for: Handlers in dbus_signals_array have been registered */
static bool dbus_signals_bound = false;

/* ========================================================================= *
 * SystemBus connection caching
 * ========================================================================= */

/** Get a SystemBus connection not bound by dsme_dbus abstractions
 *
 * To be called when D-Bus is available notification is received.
 */
static void
systembus_connect(void)
{
    DBusError err = DBUS_ERROR_INIT;

    if( !(systembus = dsme_dbus_get_connection(&err)) ) {
        dsme_log(LOG_WARNING, PFIX"can't connect to systembus: %s: %s",
                 err.name, err.message);
        goto cleanup;
    }

    xmce_tracking_init();

cleanup:

    dbus_error_free(&err);
}

/** Detach from SystemBus connection obtained via systembus_connect()
 *
 * To be called at module unload / when D-Bus no longer available
 * notification is received.
 */
static void
systembus_disconnect(void)
{
    if( systembus ) {
        xmce_tracking_quit();
        dbus_connection_unref(systembus), systembus = 0;
    }
}

/* ========================================================================= *
 * Send internal DSME messages
 * ========================================================================= */

/** Broadcast charger-is-connected status changes within DSME
 */
static void
send_charger_state(bool charging)
{
    /* Initialize to value that does not match any boolean value */
    static int prev = -1;

    if( prev == charging )
        goto cleanup;

    dsme_log(LOG_DEBUG, PFIX"broadcast: charger_state=%s", bool_repr(charging));

    DSM_MSGTYPE_SET_CHARGER_STATE msg = DSME_MSG_INIT(DSM_MSGTYPE_SET_CHARGER_STATE);
    prev = msg.connected = charging;
    broadcast_internally(&msg);

cleanup:

    return;
}

static void
send_battery_state(bool empty)
{
    dsme_log(LOG_DEBUG, PFIX"broadcast: battery_state=%s",
             empty ? "empty" : "not-empty");

    DSM_MSGTYPE_SET_BATTERY_STATE msg =
        DSME_MSG_INIT(DSM_MSGTYPE_SET_BATTERY_STATE);
    msg.empty = empty;
    broadcast_internally(&msg);
}

static void
send_dsme_state_query(void)
{
    dsme_log(LOG_DEBUG, PFIX"query: dsme_state");

    DSM_MSGTYPE_STATE_QUERY query = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);
    broadcast_internally(&query);
}

/* ========================================================================= *
 * Handle internal DSME messages
 * ========================================================================= */

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DBUS_CONNECTED");
    dsme_dbus_bind_signals(&dbus_signals_bound, dbus_signals_array);
    systembus_connect();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX"DBUS_DISCONNECT");
    systembus_disconnect();
}

DSME_HANDLER(DSM_MSGTYPE_STATE_CHANGE_IND, server, msg)
{
    dsme_log(LOG_DEBUG, PFIX"STATE_CHANGE_IND %s",
             dsme_state_repr(msg->state));
    dsme_state_set(msg->state);
}

DSME_HANDLER(DSM_MSGTYPE_SET_ALARM_STATE, conn, msg)
{
    dsme_log(LOG_DEBUG, PFIX"SET_ALARM_STATE %s",
             bool_repr(msg->alarm_set));
    alarm_active_set(msg->alarm_set);
}

/** Array of messages this module subscribes to */
module_fn_info_t message_handlers[] =
{
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_STATE_CHANGE_IND),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_SET_ALARM_STATE),
{ 0 }
};

/* ========================================================================= *
 * Module load/unload
 * ========================================================================= */

void
module_init(module_t *handle)
{
    dsme_log(LOG_DEBUG, PFIX"loading");

    /* Cache module handle */
    this_module = handle;

    /* Load configuration files */
    config_load();

    /* Query revevant internal DSME status data */
    send_dsme_state_query();
}

void
module_fini(void)
{
    dsme_log(LOG_DEBUG, PFIX"unloading");

    /* Make sure D-Bus signal handlers get unregistered */
    dsme_dbus_unbind_signals(&dbus_signals_bound, dbus_signals_array);

    /* Detach from SystemBus */
    systembus_disconnect();

    /* Do not leave active alarms behind */
    alarm_holdon_cancel();
    battery_empty_cancel_rethink();
}
