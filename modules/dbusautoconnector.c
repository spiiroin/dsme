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


#define DSME_SYSTEM_BUS_DIR "/var/run/dbus"
#define DSME_SYSTEM_BUS_FILE "system_bus_socket"
#define DSME_INOTIFY_BUF_SIZE (sizeof(struct inotify_event) + PATH_MAX + 1)


static void stop_dbus_watch(void);


static int          inotify_fd    = -1;
static guint        inotify_id    =  0;
static int          watch_fd      = -1;
static dsme_timer_t connect_timer = 0;


static void broadcast_connected(void)
{
    DSM_MSGTYPE_DBUS_CONNECT msg = DSME_MSG_INIT(DSM_MSGTYPE_DBUS_CONNECT);

    broadcast_internally(&msg);
}

static int connect_to_dbus_if_available(void* dummy)
{
    bool keep_trying = true;

    if( dsme_dbus_connect() ) {
        keep_trying = false;
        broadcast_connected();
    }

    if( !keep_trying )
        connect_timer = 0;

    return keep_trying;
}

static void try_connecting_to_dbus_until_successful(void)
{
    if (connect_to_dbus_if_available(0) != 0) {
        // D-Bus not available yet; keep trying once a second until successful
        connect_timer = dsme_create_timer(1, connect_to_dbus_if_available, 0);
    }
}


static gboolean handle_inotify_event(GIOChannel*  source,
                                     GIOCondition condition,
                                     gpointer     data)
{
    bool keep_watching = true;

    if (condition & G_IO_IN) {
        dsme_log(LOG_NOTICE, "Got D-Bus dir inotify watch event");

        char buf[DSME_INOTIFY_BUF_SIZE];
        int  n;

        n = TEMP_FAILURE_RETRY(read(inotify_fd, buf, DSME_INOTIFY_BUF_SIZE));
        if (n < sizeof(struct inotify_event)) {
            dsme_log(LOG_ERR, "Error receiving D-Bus dir inotify watch event");
            keep_watching = false;
        } else {
            struct inotify_event* event = (struct inotify_event*)&buf[0];

            if (event->mask & IN_CREATE &&
                event->len > 0          &&
                strcmp(event->name, DSME_SYSTEM_BUS_FILE) == 0)
            {
                dsme_log(LOG_INFO, "D-Bus System bus socket created; connect");
                try_connecting_to_dbus_until_successful();
                keep_watching = false; // TODO: add support for re-connect
            }
        }
    }
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        dsme_log(LOG_ERR, "ERR, HUP or NVAL on D-Bus dir inotify watch");
        keep_watching = false;
    }

    if(!keep_watching) {
        inotify_id = 0;
        stop_dbus_watch();
    }

    return keep_watching;
}

static bool set_up_watch_for_dbus(void)
{
    GIOChannel *chn =  0;

    if( inotify_id )
        goto cleanup;

    dsme_log(LOG_DEBUG, "setting up a watch for D-Bus System bus socket dir");

    if ((inotify_fd = inotify_init()) == -1) {
        dsme_log(LOG_ERR, "Error initializing inotify for D-Bus: %m");
        goto cleanup;
    }
    if ((watch_fd = inotify_add_watch(inotify_fd,
                                      DSME_SYSTEM_BUS_DIR,
                                      IN_CREATE))
                  == -1)
    {
        dsme_log(LOG_ERR, "Error adding inotify watch for D-Bus: %m");
        goto cleanup;
    }

    if ((chn = g_io_channel_unix_new(inotify_fd)) == 0) {
        dsme_log(LOG_ERR, "Error creating channel to watch for D-Bus");
        goto cleanup;
    }
    g_io_channel_set_close_on_unref(chn, FALSE);

    inotify_id = g_io_add_watch(chn,
                                G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                handle_inotify_event,
                                0);
cleanup:

    if( !inotify_id ) {
        dsme_log(LOG_ERR, "Error adding watch for D-Bus");
        stop_dbus_watch();
    }

    return inotify_id != 0;
}

static void stop_dbus_watch(void)
{
    if( inotify_id ) {
        dsme_log(LOG_DEBUG, "stopping D-Bus System bus dir watching");
        g_source_remove(inotify_id), inotify_id = 0;
    }

    if (inotify_fd != -1) {
        if (watch_fd != -1) {
            inotify_rm_watch(inotify_fd, watch_fd);
            watch_fd = -1;
        }
        close(inotify_fd);
        inotify_fd = -1;
    }
}


void module_init(module_t* handle)
{
    dsme_log(LOG_DEBUG, "dbusautoconnector.so loaded");

    if( dsme_dbus_connect() )
	broadcast_connected();
    else
        set_up_watch_for_dbus();
}

void module_fini(void)
{
    stop_dbus_watch();

    if (connect_timer) {
        dsme_destroy_timer(connect_timer), connect_timer = 0;
    }

    dsme_log(LOG_DEBUG, "dbusautoconnector.so unloaded");
}
