/**
   @file mainloop.c

   Implements DSME mainloop functionality.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
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
#include "../include/dsme/mainloop.h"
#include "../include/dsme/logging.h"

#include <stdbool.h>
#include <glib.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

typedef enum { NOT_STARTED, RUNNING, STOPPED } main_loop_state_t;

static volatile main_loop_state_t state    = NOT_STARTED;
static GMainLoop*                 the_loop = 0;

static int                        mainloop_wakeup_fd[2] = { -1, -1 };
static guint                      mainloop_wakeup_id    = 0;
static int                        mainloop_exit_code    = EXIT_SUCCESS;

static gboolean
mainloop_wakeup_cb(GIOChannel* src, GIOCondition cnd, gpointer dta)
{
    g_main_loop_quit(the_loop);

    mainloop_wakeup_id = 0;
    return FALSE;
}

static void
mainloop_wakeup_quit(void)
{
    if( mainloop_wakeup_id ) {
        g_source_remove(mainloop_wakeup_id), mainloop_wakeup_id = 0;
    }

    if( mainloop_wakeup_fd[0] != -1 ) {
        close(mainloop_wakeup_fd[0]), mainloop_wakeup_fd[0] = -1;
    }

    if( mainloop_wakeup_fd[1] != -1 ) {
        close(mainloop_wakeup_fd[1]), mainloop_wakeup_fd[1] = -1;
    }
}

static bool
mainloop_wakeup_init(void)
{
    GIOChannel *chn = 0;

    if( mainloop_wakeup_id )
        goto cleanup;

    /* create a pipe for waking up the main thread */
    if( pipe(mainloop_wakeup_fd) == - 1) {
        dsme_log(LOG_CRIT, "error creating wake up pipe: %m");
        goto cleanup;
    }

    /* set writing end of the pipe to non-blocking mode */
    errno = 0;
    int flags = fcntl(mainloop_wakeup_fd[1], F_GETFL);
    if( flags == -1 && errno != 0 ) {
        dsme_log(LOG_CRIT, "error getting flags for wake up pipe: %m");
        goto cleanup;
    }

    if( fcntl(mainloop_wakeup_fd[1], F_SETFL, flags | O_NONBLOCK) == -1 ) {
        dsme_log(LOG_CRIT, "error setting wake up pipe to non-blocking: %m");
        goto cleanup;
    }

    /* set up an I/O watch for the wake up pipe */
    if( !(chn = g_io_channel_unix_new(mainloop_wakeup_fd[0])) ) {
        goto cleanup;
    }

    mainloop_wakeup_id = g_io_add_watch(chn,
                                        G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                        mainloop_wakeup_cb, 0);

cleanup:
    if( chn )
        g_io_channel_unref(chn);

    if( !mainloop_wakeup_id )
        mainloop_wakeup_quit();

    return mainloop_wakeup_id != 0;
}

void
dsme_main_loop_run(void (*iteration)(void))
{
    if (state == NOT_STARTED) {
        state = RUNNING;

        if (!(the_loop = g_main_loop_new(0, FALSE)) ||
            !mainloop_wakeup_init())
        {
            // TODO: crash and burn
            exit(EXIT_FAILURE);
        }

        GMainContext* ctx = g_main_loop_get_context(the_loop);

        while (state == RUNNING) {
            if (iteration) {
                iteration();
            }
            if (state == RUNNING) {
                (void)g_main_context_iteration(ctx, TRUE);
            }
        }

        mainloop_wakeup_quit();

        g_main_loop_unref(the_loop), the_loop = 0;
    }
}

void
dsme_main_loop_quit(int exit_code)
{
    /* This function is used from signal handlers and
     * thus must remain Async-signal-safe. */

    if (state == RUNNING) {
        state = STOPPED;

        if (mainloop_exit_code < exit_code) {
            mainloop_exit_code = exit_code;
        }

        while( write(mainloop_wakeup_fd[1], "*", 1) == -1) {
            if( errno == EINTR )
                continue;
            _exit(EXIT_FAILURE);
        }
    }
}

int
dsme_main_loop_exit_code(void)
{
    return mainloop_exit_code;
}
