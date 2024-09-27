/**
   @file dbusautoconnector.c

   Automatically connect Dsme to D-Bus System Bus when it is available.
   <p>
   Copyright (C) 2010 Nokia Corporation.
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

#include "dsme_dbus.h"
#include "dbusproxy.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "../include/dsme/timers.h"

#include <dsme/protocol.h>
#include <unistd.h>
#include <errno.h>
#include <sys/inotify.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#define PFIX "dbusautoconnector: "

#define DSME_SYSTEM_BUS_DIR   "/var/run/dbus"
#define DSME_SYSTEM_BUS_FILE  "system_bus_socket"
#define DSME_SYSTEM_BUS_PATH  DSME_SYSTEM_BUS_DIR"/"DSME_SYSTEM_BUS_FILE

/* ========================================================================= *
 * Types
 * ========================================================================= */

typedef enum
{
    BUS_STATE_UNKNOWN,
    BUS_STATE_MISSING,
    BUS_STATE_PRESENT,
} bus_state_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

static void     connect_request        (void);

static int      connect_timer_cb       (void *dummy);
static void     connect_timer_start    (void);
static void     connect_timer_stop     (void);

static void     systembus_state_update (void);

static gboolean systembus_watcher_cb   (GIOChannel *src, GIOCondition cnd, gpointer dta);
static bool     systembus_watcher_start(void);
static void     systembus_watcher_stop (void);

void            module_init            (module_t *handle);
void            module_fini            (void);

/* ========================================================================= *
 * Data
 * ========================================================================= */

/** Cached module handle for this plugin */
static const module_t *this_module = 0;

static bus_state_t  systembus_state = BUS_STATE_UNKNOWN;

static dsme_timer_t connect_timer_id = 0;

static int          systembus_watcher_fd = -1;
static int          systembus_watcher_wd = -1;
static guint        systembus_watcher_id =  0;

/* ========================================================================= *
 * Functions
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * connect_request
 * ------------------------------------------------------------------------- */

static void
connect_request(void)
{
    DSM_MSGTYPE_DBUS_CONNECT msg = DSME_MSG_INIT(DSM_MSGTYPE_DBUS_CONNECT);

    modules_broadcast_internally(&msg);
}

/* ------------------------------------------------------------------------- *
 * connect_timer
 * ------------------------------------------------------------------------- */

static int
connect_timer_cb(void* dummy)
{
    dsme_log(LOG_DEBUG, PFIX "Connect timer: triggered");

    connect_request();

    /* Repeat forever until we get either CONNECTED or DISCONNECT event */
    return TRUE;
}

static void
connect_timer_start(void)
{
    if( connect_timer_id )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX "Connect timer: start");

    connect_timer_id = dsme_create_timer_seconds(1, connect_timer_cb, 0);

    connect_request();

EXIT:
    return;
}

static void
connect_timer_stop(void)
{
    if( connect_timer_id ) {
        dsme_log(LOG_DEBUG, PFIX "Connect timer: stop");

        dsme_destroy_timer(connect_timer_id), connect_timer_id = 0;
    }
}

/* ------------------------------------------------------------------------- *
 * systembus_state
 * ------------------------------------------------------------------------- */

static void
systembus_state_update(void)
{
    int prev = systembus_state;

    if( access(DSME_SYSTEM_BUS_PATH, F_OK) == 0 )
        systembus_state = BUS_STATE_PRESENT;
    else
        systembus_state = BUS_STATE_MISSING;

    if( prev == systembus_state )
        goto EXIT;

    dsme_log(LOG_DEBUG, PFIX "SystemBus socket exists: %d -> %d",
             prev, systembus_state);

    if( systembus_state == BUS_STATE_PRESENT )
        connect_timer_start();
    else
        connect_timer_stop();

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * systembus_watcher
 * ------------------------------------------------------------------------- */

static inline void *lea(const void *base, ssize_t offs)
{
    return offs + (char *)base;
}

static gboolean
systembus_watcher_cb(GIOChannel *src, GIOCondition cnd, gpointer dta)
{
    const module_t *caller = modulebase_enter_module(this_module);
    bool keep_watching = false;
    bool update = false;

    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));

    if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
        dsme_log(LOG_ERR, PFIX "SystemBus watch: ERR, HUP or NVAL condition");
        goto EXIT;
    }

    if( cnd & G_IO_IN ) {
        int todo = read(systembus_watcher_fd, buf, sizeof buf);

        if( todo == -1 ) {
            if( errno == EAGAIN || errno == EINTR )
                keep_watching = true;
            else
                dsme_log(LOG_ERR, PFIX "SystemBus watch: read error: %m");
            goto EXIT;
        }

        struct inotify_event *eve = lea(buf, 0);

        while( todo > 0 ) {
            if( todo < sizeof *eve ) {
                dsme_log(LOG_ERR, PFIX "SystemBus watch: truncated event");
                goto EXIT;
            }

            int size = sizeof *eve + eve->len;

            if( todo < size ) {
                dsme_log(LOG_ERR, PFIX "SystemBus watch: oversized event");
                goto EXIT;
            }

            if( eve->len > 0 && !strcmp(eve->name, DSME_SYSTEM_BUS_FILE) )
                update = true;

            eve = lea(eve, size), todo -= size;
        }
    }

    keep_watching = true;

    if( update )
        systembus_state_update();

EXIT:
    if( !keep_watching ) {
        systembus_watcher_id = 0;
        systembus_watcher_stop();
    }

    modulebase_enter_module(caller);
    return keep_watching;
}

static bool
systembus_watcher_start(void)
{
    GIOChannel *chn =  0;

    if( systembus_watcher_id )
        goto cleanup;

    dsme_log(LOG_DEBUG, PFIX "SystemBus watch: starting");

    if( (systembus_watcher_fd = inotify_init()) == -1 ) {
        dsme_log(LOG_ERR, PFIX "SystemBus watch: inotify init: %m");
        goto cleanup;
    }

    uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;

    systembus_watcher_wd = inotify_add_watch(systembus_watcher_fd,
                                             DSME_SYSTEM_BUS_DIR, mask);

    if( systembus_watcher_wd == -1 ) {
        dsme_log(LOG_ERR, PFIX "SystemBus watch: add inotify watch: %m");
        goto cleanup;
    }

    if( !(chn = g_io_channel_unix_new(systembus_watcher_fd)) ) {
        dsme_log(LOG_ERR, PFIX "SystemBus watch: creating io channel failed");
        goto cleanup;
    }

    systembus_watcher_id = g_io_add_watch(chn,
                                          G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                          systembus_watcher_cb, 0);
    if( !systembus_watcher_id )
        dsme_log(LOG_ERR, PFIX "SystemBus watch: adding io watch failed");

cleanup:
    if( !systembus_watcher_id )
        systembus_watcher_stop();

    return systembus_watcher_id != 0;
}

static void
systembus_watcher_stop(void)
{
    if( systembus_watcher_id ) {
        dsme_log(LOG_DEBUG, PFIX "SystemBus watch: stopping");
        g_source_remove(systembus_watcher_id), systembus_watcher_id = 0;
    }

    if( systembus_watcher_fd != -1 ) {
        if( systembus_watcher_wd != -1 ) {
            inotify_rm_watch(systembus_watcher_fd, systembus_watcher_wd),
                systembus_watcher_wd = -1;
        }

        close(systembus_watcher_fd), systembus_watcher_fd = -1;
    }
}

/* ------------------------------------------------------------------------- *
 * module
 * ------------------------------------------------------------------------- */

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECTED, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX "DBUS_CONNECTED");
    connect_timer_stop();
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
    dsme_log(LOG_DEBUG, PFIX "DBUS_DISCONNECT");
    connect_timer_stop();
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECTED),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
  { 0 }
};

void module_init(module_t* handle)
{
    dsme_log(LOG_DEBUG, PFIX "loaded");

    this_module = handle;

    systembus_watcher_start();
    systembus_state_update();
}

void module_fini(void)
{
    systembus_watcher_stop();

    connect_timer_stop();

    dsme_log(LOG_DEBUG, PFIX "unloaded");
}
