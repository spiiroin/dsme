/**
   @file diskmonitor.c
   Periodically monitors the disks and sends a message if the disk space usage
   exceeds the use limits.

   <p>
   Copyright (C) 2011 Nokia Corporation.
   Copyright (C) 2013-2017 Jolla Ltd

   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
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

// to send the init_done signal:
// dbus-send --system --type=signal /com/nokia/startup/signal com.nokia.startup.signal.init_done

// to request a disk space check:
// dbus-send --system --print-reply --dest=com.nokia.diskmonitor /com/nokia/diskmonitor/request com.nokia.diskmonitor.request.req_check

#define LOGPFIX "diskmonitor: "

#include <iphbd/iphb_internal.h>

#include "dsme_dbus.h"
#include "dbusproxy.h"

#include "diskmonitor.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "heartbeat.h"

#include <sys/time.h>
#include <sys/statfs.h>

#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mntent.h>

#ifndef __GLIBC__
#include <paths.h>
#endif

#include <glib.h>

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

/** Bookkeeping data for a mount point */
typedef struct
{
    /** Mountpoint path */
    gchar            *mntpoint;

    /** Free space [MB] */
    int               mb_avail;

    /** Warning limit [MB] */
    int               mb_limit;

    /** Logical disk use state */
    diskspace_state_t state;

    /** Last scan id
     *
     * Used for avoiding multiple checks in case getmntent_r() lists
     * some mountpoints more than once.
     *
     * See #diskuse_evaluate() and #diskmon_check_disk_usage().
     */
    unsigned          check_tag;
} diskuse_t;

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** The well-known D-Bus name of the diskmonitor service */
static const char diskmonitor_service[]               = "com.nokia.diskmonitor";

/** D-Bus interface name for diskmonitor requests */
static const char diskmonitor_interface_req[]         = "com.nokia.diskmonitor.request";

/** D-Bus interface name for diskmonitor signals */
static const char diskmonitor_interface_sig[]         = "com.nokia.diskmonitor.signal";

/** D-Bus object path for diskmonitor requests */
static const char diskmonitor_object_req[]            = "/com/nokia/diskmonitor/request";

/** D-Bus object path for diskmonitor signals */
static const char diskmonitor_object_sig[]            = "/com/nokia/diskmonitor/signal";

/** D-Bus method for requesting immediate check */
static const char diskmonitor_req_check[]             = "req_check";

/** D-Bus signal sent when logical diskspace changes */
static const char diskmonitor_sig_disk_space_state[]  = "disk_space_state_ind";

/** Check interval when device is active [seconds] */
static const int INTERVAL_WHEN_ACTIVE       =   60;

/** Check interval when device is inactive [seconds] */
static const int INTERVAL_WHEN_INACTIVE     = 1800;

/** Maximum check delay after device gets activated [seconds] */
static const int INTERVAL_WHEN_ACTIVATED    =   60;

/** Limit for ignoring too frequent requests over D-Bus [seconds] */
static const int INTERVAL_REQUEST_THRESHOLD =    5;

/** Maximum IPHB wakeup latency [seconds]
 *
 * Should be >= heartbeat interval, which is 12 seconds */
static const int INTERVAL_WAKEUP_LATENCY    =   12;

/** Path to diskmonitor configuration file
 *
 * See #diskmon_load_config() for defails.
 */
static const char diskmon_config[] = "/etc/dsme/diskmonitor.conf";

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// UTILITY_FUNCTIONS

static bool   at_ascii_white(const char *str);
static bool   at_ascii_black(const char *str);
static char  *slice_token   (char *pos, char **ppos);
static time_t get_boottime  (void);

// DISKSPACE_STATE_FUNCTIONS

const char *diskspace_state_repr(diskspace_state_t state);

// DISKUSE_FUNCTIONS

static diskuse_t         *diskuse_create      (const char *mntpoint);
static void               diskuse_delete      (diskuse_t *self);
static void               diskuse_delete_cb   (gpointer data);

static void               diskuse_set_limit   (diskuse_t *self, int percent_free, int mb_free);
static diskspace_state_t  diskuse_get_state   (const diskuse_t *self);
static diskspace_state_t  diskuse_update_state(diskuse_t *self);

static void               diskuse_evaluate    (diskuse_t *self, unsigned check_tag);

// DISKMONITOR_FUNCTIONS

static diskuse_t *diskmon_get_mountpoint  (const char *mntpoint);
static diskuse_t *diskmon_add_mountpoint  (const char *mntpoint, int percent_free, int mb_free);

static bool       diskmon_load_config     (void);
static void       diskmon_free_config     (void);
static void       diskmon_use_defaults    (void);

static int        diskmon_get_interval    (void);
static void       diskmon_schedule_wakeup (void);
static void       diskmon_check_disk_usage(void);

// DBUS_HOOKS

static void handle_check_req_cb     (const DsmeDbusMessage *request, DsmeDbusMessage **reply);
static void handle_init_done_sig_cb (const DsmeDbusMessage *ind);
static void handle_inactivity_sig_cb(const DsmeDbusMessage *sig);

// PLUGIN_HOOKS

void module_init(module_t *module);
void module_fini(void);

/* ========================================================================= *
 * STATE_DATA
 * ========================================================================= */

/** Flag for: init_done has been reached */
static bool init_done_received           = false;

/** Flag for: device is in active use */
static bool device_active                = false;

/** Timestamp of the last checkup [BOOTTIME] */
static time_t last_check_time            = 0;

/** Timestamp of the next checkup [BOOTTIME] */
static time_t next_check_time            = 0;

/** List of currently monitored mount points [diskuse_t *] */
static GSList *diskmon_limit_list = 0;

/** Flag for: dbus method call handlers have been installed */
static bool dbus_methods_bound           = false;

/** Array of diskmonitor method call handlers */
static const dsme_dbus_binding_t dbus_methods_array[] =
{
    // method calls
    {
        .method = handle_check_req_cb,
        .name   = diskmonitor_req_check,
        .args   = ""
    },
    // sentinel
    {
        .name   = 0,
    },
};

/** Flag for: dbus broadcast info has been installed */
static bool dbus_broadcast_bound           = false;

/** Array of signals that can be broadcast */
static const dsme_dbus_binding_t dbus_broadcast_array[] =
{
    // outbound signals
    {
        .name   = diskmonitor_sig_disk_space_state,
        .args   =
            "    <arg name=\"mount_point\" type=\"s\"/>\n"
            "    <arg name=\"diskspace_state\" type=\"i\"/>\n"
   },
    // sentinel
    {
        .name   = 0,
    },
};

/** Flag for: dbus signal handlers have been installed */
static bool dbus_signals_bound           = false;

/** Array of D-Bus signal handlers */
static const dsme_dbus_signal_binding_t dbus_signals_array[] =
{
    { handle_init_done_sig_cb,  "com.nokia.startup.signal", "init_done" },
    { handle_inactivity_sig_cb, "com.nokia.mce.signal",     "system_inactivity_ind" },
    { 0, 0 }
};

/* ========================================================================= *
 * UTILITY_FUNCTIONS
 * ========================================================================= */

/** ASCII / UTF-8 compatible white-space predicate
 */
static bool
at_ascii_white(const char *str)
{
    int ch = *(unsigned char *)str;
    return (ch > 0) && (ch <= 32);
}

/** ASCII / UTF-8 compatible non-white-space predicate
 */
static bool
at_ascii_black(const char *str)
{
    int ch = *(unsigned char *)str;
    return ch > 32;
}

/** Slice white-space separated token from ASCII / UTF-8 string
 *
 * If first non-white character found is '#', the rest of the
 * line is assumed to be comment and skipped over.
 */
static char *
slice_token(char *pos, char **ppos)
{
    char *beg = pos;

    while( at_ascii_white(beg) )
        ++beg;

    if( *beg == '#' )
        beg = strchr(beg, 0);

    char *end = beg;

    while( at_ascii_black(end) )
        ++end;

    if( at_ascii_white(end) )
        *end++ = 0;

    if( ppos )
        *ppos = end;

    return beg;
}

/** Get CLOCK_BOOTTIME timestamp with 1 second accuracy
 */
static time_t
get_boottime(void)
{
    struct timespec ts = { 0, 0 };
    if( clock_gettime(CLOCK_BOOTTIME, &ts) == -1 ) {
        dsme_log(LOG_ERR, LOGPFIX"CLOCK_BOOTTIME: %m");
    }
    return ts.tv_sec;
}

/* ========================================================================= *
 * DISKSPACE_STATE_FUNCTIONS
 * ========================================================================= */

/** Convert diskspace_state_t enumeration id to human readable string
 */
const char *
diskspace_state_repr(diskspace_state_t state)
{
    const char *repr = "UNKNOWN";
    switch( state ) {
    case DISKSPACE_STATE_UNSET:   repr = "UNSET";   break;
    case DISKSPACE_STATE_UNDEF:   repr = "UNDEF";   break;
    case DISKSPACE_STATE_NORMAL:  repr = "NORMAL";  break;
    case DISKSPACE_STATE_WARNING: repr = "WARNING"; break;
    default: break;
    }
    return repr;
}

/* ========================================================================= *
 * DISKUSE_FUNCTIONS
 * ========================================================================= */

/** Allocate a disk use bookkeeping object for a mountpoint
 */
static diskuse_t *
diskuse_create(const char *mntpoint)
{
    diskuse_t *self = g_malloc0(sizeof *self);

    self->mntpoint    = g_strdup(mntpoint);
    self->mb_avail    = 0;
    self->mb_limit    = 0;
    self->state       = DISKSPACE_STATE_UNSET;
    self->check_tag   = 0;

    return self;
}

/** Release a mountpoint disk use bookkeeping object
 */
static void
diskuse_delete(diskuse_t *self)
{
    if( self ) {
        g_free(self->mntpoint), self->mntpoint = 0;
        g_free(self);
    }
}

/** Callback function for releasing disk use bookkeeping objects
 */
static void
diskuse_delete_cb(gpointer data)
{
    diskuse_delete(data);
}

/** Set free diskspace low warning limit
 */
static void
diskuse_set_limit(diskuse_t *self, int percent_free, int mb_free)
{
    off_t mb_lim = 0;

    struct statfs stfs;

    memset(&stfs, 0, sizeof stfs);

    if( statfs(self->mntpoint, &stfs) == -1 ) {
        dsme_log(LOG_WARNING, LOGPFIX"%s: statfs failed: %m", self->mntpoint);
        goto EXIT;
    }

    if( stfs.f_blocks <= 0 || stfs.f_bsize <= 0 ) {
        goto EXIT;
    }

    /* Consider relative to disk size limit */
    if( percent_free > 0 ) {
        /* Percent to disk block count, rounded up */
        mb_lim = stfs.f_blocks;
        mb_lim *= percent_free;
        mb_lim += 99;
        mb_lim /= 100;
        /* Block count to megabytes, rounded up */
        mb_lim *= stfs.f_bsize;
        mb_lim += (1<<20) - 1;
        mb_lim >>= 20;
    }

    /* Consider absolute megabyte limit */
    if( mb_free > 0 ) {
        /* Absolute minimum MB free */
        if( mb_lim <= 0 || mb_lim > mb_free )
            mb_lim = mb_free;
    }

EXIT:
    self->mb_limit = (int)mb_lim;

    dsme_log(LOG_DEBUG, "%s: limit=%dMB", self->mntpoint, self->mb_limit);
}

/** Get current logical disk use state
 */
static diskspace_state_t
diskuse_get_state(const diskuse_t *self)
{
    return self->state;
}

/** Update available free space and logical disk use state
 */
static diskspace_state_t
diskuse_update_state(diskuse_t *self)
{
    off_t avail = -1;

    if( self->mb_limit <= 0 )
        goto EXIT;

    struct statfs stfs;

    memset(&stfs, 0, sizeof stfs);

    if( statfs(self->mntpoint, &stfs) == -1 ) {
        dsme_log(LOG_WARNING, LOGPFIX"%s: statfs failed: %m", self->mntpoint);
        goto EXIT;
    }

    /* MB free, rounded down */
    avail = stfs.f_bfree;
    avail *= stfs.f_bsize;
    avail >>= 20;

EXIT:
    /* Update available space */
    self->mb_avail = (int)avail;

    /* Update logical state */
    if( self->mb_avail < 0 )
        self->state = DISKSPACE_STATE_UNDEF;
    else if( self->mb_avail < self->mb_limit )
        self->state = DISKSPACE_STATE_WARNING;
    else
        self->state = DISKSPACE_STATE_NORMAL;

    dsme_log(LOG_DEBUG, "%s: avail=%dMB state=%s", self->mntpoint,
             self->mb_avail, diskspace_state_repr(self->state));

    return self->state;
}

/** Check and signal changes in logical disk use state
 */
static void
diskuse_evaluate(diskuse_t *self, unsigned check_tag)
{
    if( self->check_tag == check_tag )
        goto EXIT;

    self->check_tag = check_tag;

    dsme_log(LOG_DEBUG, LOGPFIX"check mountpoint: %s", self->mntpoint);

    diskspace_state_t prev = diskuse_get_state(self);
    diskspace_state_t curr = diskuse_update_state(self);

    if( prev == curr && curr != DISKSPACE_STATE_WARNING ) {
        /* Only warnings are repeated */
        goto EXIT;
    }

    if( prev == DISKSPACE_STATE_UNSET &&
        curr == DISKSPACE_STATE_NORMAL ) {
        /* Avoid spamming on dsme startup and do not
         * log initial UNSET->NORMAL transitions */
    }
    else
    {
      dsme_log(LOG_WARNING, "%s: avail=%dMB limit=%dmb state=%s->%s",
               self->mntpoint,
               self->mb_avail,
               self->mb_limit,
               diskspace_state_repr(prev),
               diskspace_state_repr(curr));
    }

    DSM_MSGTYPE_DISK_SPACE msg = DSME_MSG_INIT(DSM_MSGTYPE_DISK_SPACE);
    msg.diskspace_state = curr;
    modules_broadcast_internally_with_extra(&msg, strlen(self->mntpoint) + 1,
                                            self->mntpoint);

EXIT:
    return;
}

/* ========================================================================= *
 * DISKMONITOR_FUNCTIONS
 * ========================================================================= */

/** Find mount point from tracking list
 */
static diskuse_t *
diskmon_get_mountpoint(const char *mntpoint)
{
    diskuse_t *found = 0;

    if( !mntpoint )
        goto EXIT;

    for( GSList *item = diskmon_limit_list; item; item = item->next ) {
        diskuse_t *limit = item->data;
        if( g_strcmp0(limit->mntpoint, mntpoint) )
            continue;
        found = limit;
        break;
    }

EXIT:
    return found;
}

/** Add mount point to tracking list
 */
static diskuse_t *
diskmon_add_mountpoint(const char *mntpoint, int percent_free, int mb_free)
{
    diskuse_t *limit = 0;

    if( !(limit = diskmon_get_mountpoint(mntpoint)) ) {

        limit = diskuse_create(mntpoint);
        diskmon_limit_list = g_slist_prepend(diskmon_limit_list, limit);
    }

    diskuse_set_limit(limit, percent_free, mb_free);

    return limit;
}

/** Add mount points to tracking list from config file
 *
 * Format of the / sample config file:
 *
 * # Comment line
 * # 1st column: mountpoint path
 * # 2nd column: minimum free disk space in percent of total size
 * # 3rd column: minimum free disk space in megabytes
 * # The smallest larger than zero value is used as a limite by DSME
 *
 * /       10 100 # warn if rootfs has less than 10% or 100MB free
 * /tmp    30   0 # warn if tmp has less than 30% free
 * /home    0 200 # warn if home has less than 200MB free
 * foobar  10  10 # invalid: path must be absolute (and exist)
 * /run     0   0 # invalid: one of the limits must be > zero
 */
static bool
diskmon_load_config(void)
{
    bool    added = false;

    FILE   *input = 0;
    size_t  size  = 0;
    char   *data  = 0;

    if( !(input = fopen(diskmon_config, "r")) ) {
        if( errno != ENOENT )
            dsme_log(LOG_ERR, "%s: open failed: %m", diskmon_config);
        goto EXIT;
    }

    while( getline(&data, &size, input) != -1 ) {
        char *pos = data;

        if( *pos == '#' )
            continue;

        char *mntpoint = slice_token(pos, &pos);

        if( *mntpoint != '/' )
            continue;

        int percent   = strtol(slice_token(pos, &pos), 0, 0);
        int megabytes = strtol(slice_token(pos, &pos), 0, 0);

        if( percent <= 0 && megabytes <= 0 )
            continue;

        if( access(mntpoint, F_OK) == -1 )
            continue;

        diskmon_add_mountpoint(mntpoint, percent, megabytes);
        added = true;
    }

EXIT:
    free(data);

    if( input )
        fclose(input);

    return added;
}

/** Relase tracking data of all monitored mount points
 */
static void
diskmon_free_config(void)
{
    g_slist_free_full(diskmon_limit_list,
                      diskuse_delete_cb),
        diskmon_limit_list = 0;
}

/** Add default mount points to tracking list
 */
static void
diskmon_use_defaults(void)
{
    diskmon_add_mountpoint("/",     10, 200);
    diskmon_add_mountpoint("/tmp",  30, 200);
    diskmon_add_mountpoint("/run",  30, 200);
    diskmon_add_mountpoint("/home", 10, 200);
}

/** Get device state dependant checking interval
 */
static int
diskmon_get_interval(void)
{
    int interval;

    if( !last_check_time ) {
        interval = INTERVAL_WHEN_ACTIVE;
    }
    else if( device_active ) {
        interval = INTERVAL_WHEN_ACTIVE;
    }
    else {
        interval = INTERVAL_WHEN_INACTIVE;
    }
    return interval;
}

/** Schedule the next disk use checkup
 */
static void
diskmon_schedule_wakeup(void)
{
    time_t curtime  = get_boottime();
    time_t interval = diskmon_get_interval();
    time_t timeout  = curtime + interval;

    /* Never reschedule wakeups to happen at later time */
    if( next_check_time > curtime && next_check_time < timeout ) {
        dsme_log(LOG_DEBUG, LOGPFIX"skipping wakeup re-schedule");
        goto EXIT;
    }

    next_check_time = timeout;

    DSM_MSGTYPE_WAIT msg = DSME_MSG_INIT(DSM_MSGTYPE_WAIT);
    msg.req.mintime = interval;
    msg.req.maxtime = msg.req.mintime + INTERVAL_WAKEUP_LATENCY;
    msg.req.pid     = 0;
    msg.data        = 0;

    dsme_log(LOG_DEBUG, LOGPFIX"schedule next wakeup in: %d ... %d seconds",
             msg.req.mintime, msg.req.maxtime);

    modules_broadcast_internally(&msg);

EXIT:
    return;
}

/** Check status of all tracked mount points
 */
static void
diskmon_check_disk_usage(void)
{
    static unsigned check_tag = 0;

    FILE *fh = 0;

    if( !init_done_received )
        goto EXIT;

    dsme_log(LOG_DEBUG, LOGPFIX"check disk space usage");
    last_check_time = get_boottime();

    if( !(fh = setmntent(_PATH_MOUNTED, "r")) )
        goto EXIT;

    struct mntent mnt;
    char          buf[1024];

    ++check_tag;

    while( getmntent_r(fh, &mnt, buf, sizeof buf) ) {
        diskuse_t *lim = diskmon_get_mountpoint(mnt.mnt_dir);

        if( lim )
            diskuse_evaluate(lim, check_tag);
    }

EXIT:
    if( fh )
        endmntent(fh);
}

/* ========================================================================= *
 * DBUS_HOOKS
 * ========================================================================= */

/** Callback for handling incoming disk space check requests
 */
static void handle_check_req_cb(const DsmeDbusMessage* request, DsmeDbusMessage** reply)
{
    time_t seconds_from_last_check = get_boottime() - last_check_time;

    if( seconds_from_last_check >= INTERVAL_REQUEST_THRESHOLD ) {
        diskmon_check_disk_usage();

        diskmon_schedule_wakeup();
    }
    else {
        dsme_log(LOG_DEBUG,
                 LOGPFIX"only %d seconds from the last disk space check request, skip this request",
                 (int)seconds_from_last_check);
    }

    *reply = dsme_dbus_reply_new(request);
}

/** Callback for handling incoming init_done signals
 */
static void handle_init_done_sig_cb(const DsmeDbusMessage* ind)
{
    dsme_log(LOG_DEBUG, LOGPFIX"init_done received");

    init_done_received = true;
    diskmon_schedule_wakeup();
}

/** Callback for handling incoming inactivity signals
 */
static void handle_inactivity_sig_cb(const DsmeDbusMessage* sig)
{
    const bool inactive                 = dsme_dbus_message_get_bool(sig);
    const bool new_device_active_state  = !inactive;

    dsme_log(LOG_DEBUG, LOGPFIX"device %s signal received",
             new_device_active_state ? "active" : "inactive");

    if( new_device_active_state == device_active ) {
        /* no change in the inactivity state; don't adjust the schedule */
        return;
    }

    device_active = new_device_active_state;

    if( device_active ) {
        int seconds_from_last_check = get_boottime() - last_check_time;

        /* If the last check has not been done recently enough,
         * check immediately on activation. */
        if( seconds_from_last_check >= INTERVAL_WHEN_ACTIVATED ) {
            dsme_log(LOG_DEBUG, LOGPFIX"%d seconds from the last check",
                     seconds_from_last_check);
            diskmon_check_disk_usage();
        }

        /* Adjust the next wake-up to occur sooner */
        diskmon_schedule_wakeup();
    }
    else {
        /* Leave the previously programmed timer running so that
         * we do one more check in device-is-active schedule. */
    }
}

/* ========================================================================= *
 * Internal DSME event handling
 * ========================================================================= */

/** Callback for handling IPHB wakeup events
 */
DSME_HANDLER(DSM_MSGTYPE_WAKEUP, client, msg)
{
    dsme_log(LOG_DEBUG, LOGPFIX"iphb timer wakeup");

    /* Clear expecting-wakeup-at timestamp */
    next_check_time = 0;

    diskmon_check_disk_usage();

    diskmon_schedule_wakeup();
}

/** Callback for handling connected to SystemBus events
 */
DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
    dsme_log(LOG_DEBUG, LOGPFIX"DBUS_CONNECTED");

    dsme_dbus_bind_methods(&dbus_broadcast_bound,
                           diskmonitor_service,
                           diskmonitor_object_sig,
                           diskmonitor_interface_sig,
                           dbus_broadcast_array);

    dsme_dbus_bind_methods(&dbus_methods_bound,
                           diskmonitor_service,
                           diskmonitor_object_req,
                           diskmonitor_interface_req,
                           dbus_methods_array);

    dsme_dbus_bind_signals(&dbus_signals_bound, dbus_signals_array);

    /* If dsme was restarted after bootup is finished, init-done signal
     * is not going to come again -> check flag file for initial state */
    if( access("/run/systemd/boot-status/init-done", F_OK) == 0 ) {
        dsme_log(LOG_DEBUG, LOGPFIX"init_done already passed");
        init_done_received = true;
        diskmon_schedule_wakeup();
    }
}

/** Callback for handling disconnected from SystemBus
 */
DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, LOGPFIX"DBUS_DISCONNECT");
}

/** Callback for handling logical disk use state change events
 */
DSME_HANDLER(DSM_MSGTYPE_DISK_SPACE, conn, msg)
{
    const char* mount_path = DSMEMSG_EXTRA(msg);

    dsme_log(LOG_DEBUG, LOGPFIX"send %s disk space notification for: %s",
             diskspace_state_repr(msg->diskspace_state), mount_path);

    DsmeDbusMessage* sig =
        dsme_dbus_signal_new(diskmonitor_service,
                             diskmonitor_object_sig,
                             diskmonitor_interface_sig,
                             diskmonitor_sig_disk_space_state);

    dsme_dbus_message_append_string(sig, mount_path);
    dsme_dbus_message_append_int(sig, msg->diskspace_state);
    dsme_dbus_signal_emit(sig);
}

/** Array of DSME message handlers (used by the dsme plugin loader)
 */
module_fn_info_t message_handlers[] =
{
    DSME_HANDLER_BINDING(DSM_MSGTYPE_WAKEUP),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DISK_SPACE),
    { 0 }
};

/* ========================================================================= *
 * Plugin init and fini
 * ========================================================================= */

/** Plugin on-load handler
 */
void
module_init(module_t* module)
{
    dsme_log(LOG_DEBUG, "diskmonitor.so loaded");

    if( !diskmon_load_config() )
        diskmon_use_defaults();

    /* Checkups will commence once init_done is reached */
}

/** Plugin on-unload handler
 */
void
module_fini(void)
{
    dsme_dbus_unbind_methods(&dbus_broadcast_bound,
                             diskmonitor_service,
                             diskmonitor_object_sig,
                             diskmonitor_interface_sig,
                             dbus_broadcast_array);

    dsme_dbus_unbind_methods(&dbus_methods_bound,
                             diskmonitor_service,
                             diskmonitor_object_req,
                             diskmonitor_interface_req,
                             dbus_methods_array);

    dsme_dbus_unbind_signals(&dbus_signals_bound, dbus_signals_array);

    diskmon_free_config();

    dsme_log(LOG_DEBUG, "diskmonitor.so unloaded");
}
