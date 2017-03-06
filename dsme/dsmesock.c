/**
   @file dsmesock.c

   This file implements DSME side of dsme socket operations.
   <p>
   Copyright (C) 2004-2010 Nokia Corporation.
   Copyright (C) 2012-2017 Jolla Ltd.

   @author Ari Saastamoinen
   @author Semi Malinen <semi.malinen@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>
   @author Markus Lehtonen <markus.lehtonen@iki.fi>
   @author Matias Muhonen <mmu@iki.fi>
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

#include "../include/dsme/dsmesock.h"
#include "../include/dsme/logging.h"
#include "../include/dsme/modulebase.h"
#include <dsme/protocol.h>

#include <stdio.h>
#include <glib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>

static gboolean accept_client(GIOChannel*  source,
                              GIOCondition condition,
                              gpointer     p);
static gboolean handle_client(GIOChannel*  source,
                              GIOCondition condition,
                              gpointer     conn);
static void close_client(dsmesock_connection_t* conn);
static void add_client(dsmesock_connection_t* conn);
static void remove_client(dsmesock_connection_t* conn);

/* List of all connections made to listening socket */
static GSList* clients = 0;

/* iowatch for connect socket fd */
static guint listen_id = 0;

static dsmesock_callback* read_and_queue_f =  0;

/** Set iowatch id for connection
 */
static void
set_watch_id(dsmesock_connection_t *conn, guint wid)
{
    /* FIXME: the structure in libdsme should be changed, but
     * for now just store the watch id in channel pointer... */
    conn->channel = GINT_TO_POINTER(wid);
}

/** Get iowatch id from connection
 */
static guint
get_watch_id(const dsmesock_connection_t *conn)
{
    return GPOINTER_TO_UINT(conn->channel);
}

/*
 * Initialize listening socket and static variables
 * Return 0 on OK, -1 on error.
 */
int dsmesock_listen(dsmesock_callback* read_and_queue)
{
    int         fd  = -1;
    GIOChannel *chn = 0;

    /* Determine path to connect socket */
    const char *path = getenv("DSME_SOCKFILE");
    if( !path || !*path )
        path = dsmesock_default_location;

    /* Set up unix domain socket address */
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);

    if( strcmp(sa.sun_path, path) )
        goto cleanup;

    /* Create non-blocking unix domain connect socket */
    if( (fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1 )
        goto cleanup;

    errno = 0;
    int flags = fcntl(fd, F_GETFL);
    if( flags == -1 && errno != 0 )
        goto cleanup;
    flags |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1)
        goto cleanup;

    unlink(path);
    if( bind(fd, (struct sockaddr *)&sa, sizeof sa) == -1 )
        goto cleanup;

    chmod(path, 0646);

    if( listen(fd, 1) == -1 )
        goto cleanup;

    /* Setup io watch for client connects */
    if( !(chn = g_io_channel_unix_new(fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, true), fd = -1;

    listen_id = g_io_add_watch(chn,
                               G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                               accept_client, 0);

    if( !listen_id )
        goto cleanup;

    read_and_queue_f = read_and_queue;

cleanup:
    if( chn )
        g_io_channel_unref(chn);

    if( fd != -1 )
        close(fd);

    return listen_id ? 0 : -1;
}

static gboolean
accept_client(GIOChannel *src, GIOCondition cnd, gpointer aptr)
{
    (void)aptr;

    gboolean               keep_going = TRUE;
    int                    fd         = -1;
    dsmesock_connection_t *conn       = 0;
    GIOChannel            *chn        = 0;

    /* Remove watch on error conditions */
    if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
        keep_going = FALSE;
        goto cleanup;
    }

    /* Accept client connection */
    if( (fd = accept(g_io_channel_unix_get_fd(src), 0, 0)) == -1 )
        goto cleanup;

    if( !(conn = dsmesock_init(fd)) )
        goto cleanup;

    /* socket fd is now owned by conn */
    fd = -1;

    /* Get client credentials */
    int       opt = 1;
    socklen_t len = sizeof opt;
    if( setsockopt(conn->fd, SOL_SOCKET, SO_PASSCRED, &opt, len) == -1 ) {
        /* If that fails it is not fatal */
    }

    len = sizeof conn->ucred;
    if( getsockopt(conn->fd, SOL_SOCKET, SO_PEERCRED,
                   &conn->ucred, &len) == -1 )
    {
        /* if that fails, fill some bogus values */
        conn->ucred.pid =  0;
        conn->ucred.uid = -1;
        conn->ucred.gid = -1;
    }

    /* Attach iowatch to handle client input */
    if( !(chn = g_io_channel_unix_new(conn->fd)) )
        goto cleanup;

    guint wid = g_io_add_watch(chn, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                               handle_client, conn);

    if( wid == 0 )
        goto cleanup;

    set_watch_id(conn, wid);

    /* Transfer the conn ownership to the client list */
    add_client(conn), conn = 0;

cleanup:

    if( chn )
        g_io_channel_unref(chn);

    if( conn )
        close_client(conn);

    if( fd != -1 )
        close(fd);

    if( !keep_going ) {
        dsme_log(LOG_CRIT, "disabling client connect watcher");
        listen_id = 0;
    }

    return keep_going;
}

static gboolean
handle_client(GIOChannel* src, GIOCondition cnd, gpointer aptr)
{
    dsmesock_connection_t *conn = aptr;

    bool keep_connection = true;

    if( cnd & G_IO_IN ) {
        if( !read_and_queue_f )
            keep_connection = false;
        else if( !read_and_queue_f(conn) )
            keep_connection = false;
    }

    if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) )
        keep_connection = false;

    if( !keep_connection ) {
        set_watch_id(conn, 0);
        close_client(conn);
    }

    return keep_connection;
}

static void
close_client(dsmesock_connection_t* conn)
{
  if (conn) {
      remove_client(conn);

      guint wid = get_watch_id(conn);
      if( wid ) {
          g_source_remove(wid);
          set_watch_id(conn, 0);
      }

      dsmesock_close(conn);
  }
}

static void
add_client(dsmesock_connection_t* conn)
{
    clients = g_slist_prepend(clients, conn);
}

static void
remove_client(dsmesock_connection_t* conn)
{
    GSList* node = g_slist_find(clients, conn);

    if (node) {
        clients = g_slist_delete_link(clients, node);
    }
}

/*
 * Close listening socket
 * Close all client sockets
 */
void
dsmesock_shutdown(void)
{
    if( listen_id ) {
        g_source_remove(listen_id), listen_id = 0;
    }

    while( clients ) {
        close_client(clients->data);
    }
}
