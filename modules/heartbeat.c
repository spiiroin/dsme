/**
   @file heartbeat.c

   Implements DSME server periodic wake up functionality.
   <p>
   Copyright (C) 2009-2010 Nokia Corporation.
   Copyright (C) 2014-2017 Jolla Ltd.

   @author Semi Malinen <semi.malinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
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

#include "heartbeat.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/logging.h"
#include "../include/dsme/mainloop.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>

/** Cached module handle for this plugin */
static const module_t *this_module = 0;

static guint watch_id = 0;

static gboolean emit_heartbeat_message(GIOChannel*  source,
                                       GIOCondition condition,
                                       gpointer     data)
{
    const module_t *caller = enter_module(this_module);
    bool keep_going = false;

    // handle errors
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        // the wd process has probably died; remove the watch & quit
        dsme_log(LOG_CRIT, "heartbeat: I/O error or HUP, terminating");
        goto cleanup;
    }

    char    c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);

    if( n == 0 ) {
        dsme_log(LOG_CRIT, "heartbeat: unexpected EOF, terminating");
        goto cleanup;
    }

    if( n == -1 ) {
        if( errno == EINTR || errno == EAGAIN )
            keep_going = true;
        else
            dsme_log(LOG_CRIT, "heartbeat: read error: %m, terminating");
        goto cleanup;
    }

    // got a ping from the wd process; respond with a pong
    do {
        n = write(STDOUT_FILENO, "*", 1);
    } while( n == -1 && errno == EINTR );

    // send the heartbeat message
    const DSM_MSGTYPE_HEARTBEAT beat = DSME_MSG_INIT(DSM_MSGTYPE_HEARTBEAT);
    broadcast_internally(&beat);

    keep_going = true;

cleanup:

    if( !keep_going ) {
        watch_id = 0;
        dsme_main_loop_quit(EXIT_FAILURE);
    }

    enter_module(caller);
    return keep_going;
}

static bool start_heartbeat(void)
{
    // set up an I/O watch for the wake up pipe (stdin/stdout)
    GIOChannel *chn = 0;

    if( watch_id )
        goto cleanup;

    if( !(chn = g_io_channel_unix_new(STDIN_FILENO)) )
        goto cleanup;

    watch_id = g_io_add_watch(chn,
                              G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                              emit_heartbeat_message,
                              0);
cleanup:

    if( chn )
        g_io_channel_unref(chn);

    return watch_id != 0;
}

static void stop_heatbeat(void)
{
    if( watch_id ) {
        g_source_remove(watch_id), watch_id = 0;
    }
}

void module_init(module_t* handle)
{
    dsme_log(LOG_DEBUG, "heartbeat.so loaded");

    this_module = handle;

    start_heartbeat();
}

void module_fini(void)
{
    dsme_log(LOG_DEBUG, "heartbeat.so unloaded");

    stop_heatbeat();
}
