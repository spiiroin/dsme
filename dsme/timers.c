/**
   @file timers.c

   Implementation of DSME timers.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2015-2017 Jolla Ltd.

   @author Ari Saastamoinen
   @authot Semi Malinen <semi.malinen@nokia.com>
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

#include "../include/dsme/timers.h"
#include "../include/dsme/modulebase.h"
#include "../include/dsme/logging.h"

#include <glib.h>

typedef struct
{
    const module_t        *tg_module;
    guint                  tg_interval;
    dsme_timer_callback_t  tg_callback;
    void                  *tg_data;

} timergate_t;

static void
timergate_delete(timergate_t *self)
{
    if( !self )
        goto EXIT;

    dsme_log(LOG_DEBUG, "delete %ums timer from module: %s",
             self->tg_interval, module_name(self->tg_module) ?: "unknown");
    g_slice_free1(sizeof *self, self);

EXIT:
    return;
}

static void
timergate_delete_cb(gpointer aptr)
{
    timergate_delete(aptr);
}

static gboolean
timergate_timeout_cb(gpointer aptr)
{
    timergate_t *self = aptr;

    dsme_log(LOG_DEBUG, "dispatch %ums timer at module: %s",
             self->tg_interval, module_name(self->tg_module) ?: "unknown");

    const module_t *cur = enter_module(self->tg_module);
    int rc = self->tg_callback(self->tg_data);
    enter_module(cur);

    return rc != 0;
}

static dsme_timer_t
timergate_create(gint priority,
                 guint interval,
                 dsme_timer_callback_t callback,
                 void *data)
{
    guint id = 0;

    timergate_t *self = g_slice_alloc0(sizeof *self);

    self->tg_module   = current_module();
    self->tg_interval = interval;
    self->tg_callback = callback;
    self->tg_data     = data;

    dsme_log(LOG_DEBUG, "create %ums timer from module: %s",
             self->tg_interval, module_name(self->tg_module) ?: "unknown");

    if( interval > 0 ) {
        id = g_timeout_add_full(priority, interval,
                                timergate_timeout_cb,
                                self, timergate_delete_cb);
    }
    else {
        id = g_idle_add_full(priority,
                             timergate_timeout_cb,
                             self, timergate_delete_cb);
    }

    if( !id )
        timergate_delete(self);

    return id;
}

dsme_timer_t
dsme_create_timer_seconds(unsigned              seconds,
                          dsme_timer_callback_t callback,
                          void*                 data)
{
    return timergate_create(G_PRIORITY_DEFAULT,
                            seconds * 1000,
                            callback, data);
}

void
dsme_destroy_timer(dsme_timer_t timer)
{
    if( timer )
        g_source_remove(timer);
}
