/**
   @file dsme_dbus.c

   D-Bus C binding for DSME
   <p>
   Copyright (C) 2008-2010 Nokia Corporation.
   Copyright (C) 2013-2017 Jolla Ltd.

   @author Semi Malinen <semi.malinen@nokia.com>
   @author Tapio Rantala <ext-tapio.rantala@nokia.com>
   @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
   @author Jarkko Nikula <jarkko.nikula@jollamobile.com>
   @author Pekka Lundstrom <pekka.lundstrom@jollamobile.com>
   @author Kalle Jokiniemi <kalle.jokiniemi@jolla.com>
   @author Slava Monich <slava.monich@jolla.com>
   @author marko lemmetty <marko.lemmetty@jollamobile.com>

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

#include "../include/dsme/logging.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/modulebase.h"
#include <dsme/state.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================= *
 * Types & Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DsmeDbusMessage  --  Custom wrapper for D-Bus messages
 * ------------------------------------------------------------------------- */

/** Wrapper for D-Bus message
 *
 * At least in theory makes things easier by allowing handler callbacks not
 * care about connection details and use of parse/append helper functions.
 */
struct DsmeDbusMessage
{
    /** Connection to use. In practice it is always SystemBus */
    DBusConnection  *connection;

    /* D-Bus message */
    DBusMessage     *msg;

    /* Read / Append iterator used by helper functions */
    DBusMessageIter  iter;
};

// internal

static DBusMessageIter   *message_iter                      (const DsmeDbusMessage *self);
static void               message_init_read_iterator        (DsmeDbusMessage *self);
static void               message_init_append_iterator      (DsmeDbusMessage *self);
static void               message_ctor                      (DsmeDbusMessage *self, DBusConnection *con, DBusMessage *msg, bool append);
static void               message_dtor                      (DsmeDbusMessage *self);
static DsmeDbusMessage   *message_new                       (DBusConnection *con, DBusMessage *msg);
static void               message_delete                    (DsmeDbusMessage *self);
static void               message_send_and_delete           (DsmeDbusMessage *self);

// external

DsmeDbusMessage          *dsme_dbus_reply_new               (const DsmeDbusMessage *request);
DsmeDbusMessage          *dsme_dbus_reply_error             (const DsmeDbusMessage *request, const char *error_name, const char *error_message );
DsmeDbusMessage          *dsme_dbus_signal_new              (const char *path, const char *interface, const char *name);
void                      dsme_dbus_signal_emit             (DsmeDbusMessage *sig);
const char               *dsme_dbus_message_path            (const DsmeDbusMessage *msg);
char                     *dsme_dbus_endpoint_name           (const DsmeDbusMessage *request);
void                      dsme_dbus_message_append_string   (DsmeDbusMessage *msg, const char *s);
void                      dsme_dbus_message_append_int      (DsmeDbusMessage *msg, int i);
int                       dsme_dbus_message_get_int         (const DsmeDbusMessage *msg);
const char               *dsme_dbus_message_get_string      (const DsmeDbusMessage *msg);
bool                      dsme_dbus_message_get_bool        (const DsmeDbusMessage *msg);
bool                      dsme_dbus_message_get_variant_bool(const DsmeDbusMessage *msg);

/* ------------------------------------------------------------------------- *
 * Dispatcher
 * ------------------------------------------------------------------------- */

typedef struct Dispatcher Dispatcher;

typedef bool (*Dispatch)(const Dispatcher *dispatcher,
                         DBusConnection   *connection,
                         DBusMessage      *msg);

/** Object for binding incoming D-Bus messages to callback functions
 *
 * Needs to handle both signal and method call handler dispatching.
 * The implementation differences between the two are handled via type
 * specific dispatch and notify callbacks.
 */
struct Dispatcher
{
    /** DSME plugin that did the method/signal binding
     *
     * Final callback will be made in the context of the plugin that
     * originally caused the the dispatcher to be installed. */
    const module_t *module;

    /** D-Bus interface name */
    gchar          *interface;

    /** D-Bus signal / method call name */
    gchar          *member;

    /** Binding type specific dispatch callback
     *
     * Effectively this is either method_dispatcher_dispatch_cb()
     * or handler_dispatcher_dispatch_cb(). */
    Dispatch        dispatch_cb; /* method_dispatcher_dispatch_cb(), or
                                  * handler_dispatcher_dispatch_cb() */

    /** Binding type specific notification callback
     *
     * Effectively this is either of type DsmeDbusMethod or DsmeDbusHandler */
    void           *notify_cb;

};

// common

static Dispatcher        *dispatcher_new                    (Dispatch dispatch_cb, const char *interface, const char *name, void *notify_cb);
static void               dispatcher_delete                 (Dispatcher *self);
static void               dispatcher_delete_cb              (gpointer self);

// method call handlers

static bool               method_dispatcher_dispatch_cb     (const Dispatcher *self, DBusConnection *connection, DBusMessage *msg);

static Dispatcher        *method_dispatcher_new             (const char *interface, const char *name, DsmeDbusMethod notify_cb);

// signal handlers

static bool               handler_dispatcher_dispatch_cb    (const Dispatcher *self, DBusConnection *connection, DBusMessage *msg);

static Dispatcher        *handler_dispatcher_new            (const char *interface, const char *name, DsmeDbusHandler notify_cb);

/* ------------------------------------------------------------------------- *
 * DispatcherList
 * ------------------------------------------------------------------------- */

/** Helper for managing a set of Dispatcher objects
 */
typedef struct DispatcherList
{
    /** Linked list of Dispatcher objects */
    GSList *dispatchers; // -> Dispatcher *

} DispatcherList;

static bool               dispatcher_list_is_empty          (const DispatcherList *self);
static bool               dispatcher_list_dispatch          (const DispatcherList *self, DBusConnection *connection, DBusMessage *msg);
static void               dispatcher_list_add               (DispatcherList *self, Dispatcher *dispatcher);
static void               dispatcher_list_delete            (DispatcherList *self);
static DispatcherList    *dispatcher_list_new               (void);

/* ------------------------------------------------------------------------- *
 * Filter
 * ------------------------------------------------------------------------- */

typedef bool (*FilterMessageHandler)(DBusConnection *connection,
                                     DBusMessage    *msg,
                                     gpointer        context);

/** Object for attaching message filter to D-Bus connection
 *
 * Used for channeling signal messages to Client objects
 * and method call messages to Service objects
 */
typedef struct Filter
{
    /** D-Bus connection where filter callback has been installed
     *
     * In practice this is always SystemBus connection. */
    DBusConnection       *connection;

    /** Binding type specific handler callback
     *
     * In practice this is either client_filter_cb()
     * or service_filter_cb() */
    FilterMessageHandler  handler_cb;

    /** Parameter passed to handler_cb
     *
     * In practice a pointer to Client or Service object */
    void                 *context;

} Filter;

static DBusHandlerResult  filter_handle_message_cb          (DBusConnection *connection, DBusMessage *msg, gpointer filter);
static Filter            *filter_new                        (FilterMessageHandler handler_cb, void *context);
static void               filter_delete                     (Filter *self);

/* ------------------------------------------------------------------------- *
 * Service
 * ------------------------------------------------------------------------- */

/** Object for registering D-Bus service name and method call handers
 */
typedef struct Service
{
    /** "Well known" D-Bus service name associated with this service */
    gchar          *name;

    /** Flag for: We have acquired the service name from dbus daemon */
    bool            reserved;

    /** Filter object for this service */
    Filter         *filter;

    /** Method call dispatchers for this service */
    DispatcherList *methods;
} Service;

static bool               service_has_methods               (const Service *self);
static bool               service_filter_cb                 (DBusConnection *connection, DBusMessage *msg, gpointer service);
static void               service_bind                      (Service *self, const char *interface, const char *name, DsmeDbusMethod notify_cb);
static void               service_unbind                    (Service *self, const char *interface, const char *name, DsmeDbusMethod notify_cb);
static bool               service_reserve_name              (Service *self);
static void               service_release_name              (Service *self);
static Service           *service_new                       (const char *name);
static void               service_delete                    (Service *self);
static void               service_delete_cb                 (gpointer self);

/* ------------------------------------------------------------------------- *
 * Server
 * ------------------------------------------------------------------------- */

/* (Singleton) Object for binding D-Bus method call message handlers
 */
typedef struct Server
{
    /** Service name to Service object lookup table */
    GData *services; // [well_known_name] -> Service *
} Server;

static bool               server_bind                       (Server *self, const char *service, const char *interface, const char *name, DsmeDbusMethod notify_cb);
static void               server_unbind                     (Server *self, const char *service_name, const char *interface, const char *member, DsmeDbusMethod notify_cb);
static Server            *server_new                        (void);
static Server            *server_instance                   (void);
static void               server_delete                     (Server *self);

/* ------------------------------------------------------------------------- *
 * Client
 * ------------------------------------------------------------------------- */

/** (Singleton) Object for binding D-Bus signal message handlers
 */
typedef struct Client
{
    /** Filter object for this Client */
    Filter         *filter;

    /** Signal dispatchers for this Client */
    DispatcherList *handlers;
} Client;

static char              *client_match                      (const char *interface, const char *member);
static bool               client_filter_cb                  (DBusConnection *connection, DBusMessage *msg, gpointer client);
static bool               client_bind                       (Client *self, const char *interface, const char *member, DsmeDbusHandler handler);
static void               client_unbind                     (Client *self, const char *interface, const char *member, DsmeDbusHandler handler);
static Client            *client_new                        (void);
static Client            *client_instance                   (void);
static void               client_delete                     (Client *self);

/* ------------------------------------------------------------------------- *
 * DSME_DBUS
 * ------------------------------------------------------------------------- */

// internal

static bool               dsme_dbus_is_enabled              (void);
static const char        *dsme_dbus_caller_name             (void);
static bool               dsme_dbus_connection_is_open      (DBusConnection *con);
static bool               dsme_dbus_bus_get_unix_process_id (DBusConnection *conn, const char *name, pid_t *pid);
static const char        *dsme_dbus_get_type_name           (int type);
static bool               dsme_dbus_check_arg_type          (DBusMessageIter *iter, int want_type);
static DBusHandlerResult  dsme_dbus_connection_filter_cb    (DBusConnection *con, DBusMessage *msg, void *aptr);
static DBusConnection    *dsme_dbus_try_to_connect          (DBusError *err);
static void               dsme_dbus_disconnect              (void);

// external

DBusConnection           *dsme_dbus_get_connection          (DBusError *err);
bool                      dsme_dbus_is_available            (void);
void                      dsme_dbus_bind_methods            (bool *bound, const dsme_dbus_binding_t *bindings, const char *service, const char *interface);
void                      dsme_dbus_unbind_methods          (bool *bound, const dsme_dbus_binding_t *bindings, const char *service, const char *interface);
void                      dsme_dbus_bind_signals            (bool *bound, const dsme_dbus_signal_binding_t *bindings);
void                      dsme_dbus_unbind_signals          (bool *bound, const dsme_dbus_signal_binding_t *bindings);
void                      dsme_dbus_cleanup                 (void);

/* ========================================================================= *
 * FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DsmeDbusMessage
 * ------------------------------------------------------------------------- */

static DBusMessageIter *
message_iter(const DsmeDbusMessage *self)
{
    /* The problem is that parse heler API functions claim to
     * to take const DsmeDbusMessage pointer, while the iterator
     * data naturally needs to be modified.
     *
     * To minimize changes needed outside this module, contain
     * the broken const-promise in single location i.e. here.
     */
    return (DBusMessageIter *)&self->iter;
}

static void
message_init_read_iterator(DsmeDbusMessage *self)
{
    if( self && self->msg )
        dbus_message_iter_init(self->msg, message_iter(self));
}

static void
message_init_append_iterator(DsmeDbusMessage *self)
{
    if( self && self->msg )
        dbus_message_iter_init_append(self->msg, message_iter(self));
}

static void
message_ctor(DsmeDbusMessage *self,
             DBusConnection  *con,
             DBusMessage     *msg,
             bool             append)
{
    self->connection = con ? dbus_connection_ref(con) : 0;
    self->msg        = msg ? dbus_message_ref(msg)    : 0;

    if( append )
        message_init_append_iterator(self);
    else
        message_init_read_iterator(self);
}

static void
message_dtor(DsmeDbusMessage *self)
{
    if( self->msg ) {
        dbus_message_unref(self->msg),
            self->msg = 0;
    }

    if( self->connection )  {
        dbus_connection_unref(self->connection),
            self->connection = 0;
    }
}

static DsmeDbusMessage *
message_new(DBusConnection *con, DBusMessage *msg)
{
    DsmeDbusMessage *self = 0;

    if( con && msg ) {
        self = g_new(DsmeDbusMessage, 1);
        message_ctor(self, con, msg, true);
    }

    return self;
}

static void
message_delete(DsmeDbusMessage *self)
{
    if( self ) {
        message_dtor(self);
        g_free(self);
    }
}

static void
message_send_and_delete(DsmeDbusMessage *self)
{
    if( self ) {
        if( dsme_dbus_connection_is_open(self->connection) ) {
            dbus_connection_send(self->connection, self->msg, 0);
            dbus_connection_flush(self->connection);
        }
        message_delete(self);
    }
}

DsmeDbusMessage *
dsme_dbus_reply_new(const DsmeDbusMessage *request)
{
    DsmeDbusMessage *rsp = 0;
    DBusMessage     *msg = 0;

    if( !request )
        goto EXIT;

    msg = dbus_message_new_method_return(request->msg);
    rsp = message_new(request->connection, msg);

EXIT:

    if( msg )
        dbus_message_unref(msg);

    return rsp;
}

DsmeDbusMessage *
dsme_dbus_reply_error(const DsmeDbusMessage *request,
                      const char            *error_name,
                      const char            *error_message)
{
    DsmeDbusMessage *rsp = 0;
    DBusMessage     *msg = 0;

    if( !request || !error_name || !error_message)
        goto EXIT;

    msg = dbus_message_new_error(request->msg, error_name, error_message);
    rsp = message_new(request->connection, msg);

EXIT:

    if( msg )
        dbus_message_unref(msg);

    return rsp;
}

DsmeDbusMessage *
dsme_dbus_signal_new(const char *path,
                     const char *interface,
                     const char *name)
{
    DsmeDbusMessage *sig = 0;
    DBusMessage     *msg = 0;
    DBusConnection  *con = 0;

    if( !path || !interface || !name )
        goto EXIT;

    if( !dsme_dbus_is_enabled() ) {
        dsme_log(LOG_ERR, "signal %s.%s send attempt from %s while dbus functionality disabled",
                 interface, name, dsme_dbus_caller_name());
        goto EXIT;
    }

    if( !(con = dsme_dbus_get_connection(0)) )
        goto EXIT;

    msg = dbus_message_new_signal(path, interface, name);
    sig = message_new(con, msg);

EXIT:

    if( msg )
        dbus_message_unref(msg);

    if( con )
        dbus_connection_unref(con);

    return sig;
}

void
dsme_dbus_signal_emit(DsmeDbusMessage *sig)
{
    if( sig ) {
        message_send_and_delete(sig);
    }
}

const char *
dsme_dbus_message_path(const DsmeDbusMessage *self)
{
    const char *path = 0;

    if( self && self->msg )
        path = dbus_message_get_path(self->msg);

    return path ?: "";
}

char *
dsme_dbus_endpoint_name(const DsmeDbusMessage *request)
{
    char *name = 0;

    if( !request || !request->msg ) {
        name = strdup("(null request)");
        goto EXIT;
    }

    const char *sender = dbus_message_get_sender(request->msg);

    if( !sender ) {
        name = strdup("(null sender)");
        goto EXIT;
    }

    pid_t pid = -1;

    // TODO: it is risky that we are blocking
    if( !dsme_dbus_bus_get_unix_process_id(request->connection, sender, &pid) ) {
        name = strdup("(could not get pid)");
        goto EXIT;
    }

    if( !(name = endpoint_name_by_pid(pid)) )
        name = strdup("(could not get name)");

EXIT:
    return name;
}

void
dsme_dbus_message_append_string(DsmeDbusMessage *self, const char *val)
{
    if( self ) {
        dbus_message_iter_append_basic(message_iter(self), DBUS_TYPE_STRING, &val);
    }
}

void
dsme_dbus_message_append_int(DsmeDbusMessage *self, int val)
{
    if( self ) {
        dbus_int32_t dta = val;
        dbus_message_iter_append_basic(message_iter(self), DBUS_TYPE_INT32, &dta);
    }
}

int
dsme_dbus_message_get_int(const DsmeDbusMessage *self)
{
    // FIXME: caller can't tell apart zero from error
    dbus_int32_t dta = 0;

    if( self ) {
        if( dsme_dbus_check_arg_type(message_iter(self), DBUS_TYPE_INT32) ) {
            dbus_message_iter_get_basic(message_iter(self), &dta);
        }
        dbus_message_iter_next(message_iter(self));
    }

    return dta;
}

const char *
dsme_dbus_message_get_string(const DsmeDbusMessage *self)
{
    // FIXME: caller can't tell apart empty string from error
    const char *dta = "";

    if( self ) {
        if( dsme_dbus_check_arg_type(message_iter(self), DBUS_TYPE_STRING) ) {
            dbus_message_iter_get_basic(message_iter(self), &dta);
        }
        dbus_message_iter_next(message_iter(self));
    }

    return dta;
}

bool
dsme_dbus_message_get_bool(const DsmeDbusMessage *self)
{
    // FIXME: caller can't tell apart FALSE from error
    dbus_bool_t dta = FALSE;

    if( self ) {
        if( dsme_dbus_check_arg_type(message_iter(self), DBUS_TYPE_BOOLEAN) ) {
            dbus_message_iter_get_basic(message_iter(self), &dta);
        }
        dbus_message_iter_next(message_iter(self));
    }

    return dta;
}

bool
dsme_dbus_message_get_variant_bool(const DsmeDbusMessage *self)
{
    // FIXME: caller can't tell apart FALSE from error
    dbus_bool_t dta = FALSE;

    if( self ) {
        DBusMessageIter subiter;

        if( dsme_dbus_check_arg_type(message_iter(self), DBUS_TYPE_VARIANT) ) {
            dbus_message_iter_recurse (message_iter(self), &subiter);
            if( dsme_dbus_check_arg_type(&subiter, DBUS_TYPE_BOOLEAN) ) {
                dbus_message_iter_get_basic(&subiter, &dta);
            }
        }

        dbus_message_iter_next(message_iter(self));
    }

    return dta;
}

/* ------------------------------------------------------------------------- *
 * Dispatcher -- common
 * ------------------------------------------------------------------------- */

static Dispatcher *
dispatcher_new(Dispatch    dispatch_cb,
               const char *interface,
               const char *member,
               void       *notify_cb)
{
    Dispatcher *self = g_new(Dispatcher, 1);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    self->module      = current_module();
    self->dispatch_cb = dispatch_cb;
    self->interface   = g_strdup(interface);
    self->member      = g_strdup(member);
    self->notify_cb   = notify_cb;

    return self;
}

static void
dispatcher_delete(Dispatcher *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

        self->module      = 0;
        self->dispatch_cb = 0;
        self->notify_cb   = 0;

        g_free(self->member),
            self->member = 0;

        g_free(self->interface),
            self->interface = 0;

        g_free(self);
    }
}

static void
dispatcher_delete_cb(gpointer self)
{
    dispatcher_delete(self);
}

/* ------------------------------------------------------------------------- *
 * Dispatcher -- method calls
 * ------------------------------------------------------------------------- */

static bool
method_dispatcher_dispatch_cb(const Dispatcher *self,
                              DBusConnection   *connection,
                              DBusMessage      *msg)
{
    bool dispatched = false;

    if( dbus_message_is_method_call(msg, self->interface, self->member) ) {
        dsme_log(LOG_DEBUG, "dispatch method %s.%s",
                 self->interface, self->member);

        DsmeDbusMessage *reply  = 0;
        DsmeDbusMethod   notify = self->notify_cb;

        DsmeDbusMessage  req;
        message_ctor(&req, connection, msg, false);

        enter_module(self->module);
        notify(&req, &reply);
        leave_module();

        message_dtor(&req);

        if( reply ) {
            dsme_log(LOG_DEBUG, "replying method %s.%s",
                     self->interface, self->member);
            message_send_and_delete(reply);
        }

        dispatched = true;
    }

    return dispatched;
}

static Dispatcher *
method_dispatcher_new(const char     *interface,
                      const char     *member,
                      DsmeDbusMethod  notify_cb)
{
    Dispatcher *self = dispatcher_new(method_dispatcher_dispatch_cb,
                                      interface, member, notify_cb);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);
    return self;
}

/* ------------------------------------------------------------------------- *
 * Dispatcher -- signals
 * ------------------------------------------------------------------------- */

static bool
handler_dispatcher_dispatch_cb(const Dispatcher *self,
                               DBusConnection   *connection,
                               DBusMessage      *msg)
{
    bool dispatched = false;

    if( dbus_message_is_signal(msg, self->interface, self->member) ) {

        dsme_log(LOG_DEBUG, "dispatch signal %s.%s",
                 self->interface, self->member);

        DsmeDbusMessage ind;
        message_ctor(&ind, connection, msg, false);

        enter_module(self->module);
        DsmeDbusHandler notify = self->notify_cb;
        notify(&ind);
        leave_module();

        message_dtor(&ind);

        dispatched = true;
    }

    return dispatched;
}

static Dispatcher *
handler_dispatcher_new(const char     *interface,
                       const char     *member,
                       DsmeDbusHandler notify_cb)
{
    Dispatcher *self = dispatcher_new(handler_dispatcher_dispatch_cb,
                                      interface, member, notify_cb);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);
    return self;
}

/* ------------------------------------------------------------------------- *
 * DispatcherList
 * ------------------------------------------------------------------------- */

static bool
dispatcher_list_is_empty(const DispatcherList *self)
{
    return self->dispatchers == 0;
}

static bool
dispatcher_list_dispatch(const DispatcherList *self,
                         DBusConnection       *connection,
                         DBusMessage          *msg)
{
    bool dispatched = false;

    for( GSList *item = self->dispatchers; item; item = g_slist_next(item)) {
        Dispatcher  *dispatcher = item->data;

        if( dispatcher->dispatch_cb(dispatcher, connection, msg) ) {
            dispatched = true;

            /* Method calls should have only one handler.
             * Stop after suitable one has been found. */
            int msg_type = dbus_message_get_type(msg);
            if( msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL )
                break;
        }
    }

    return dispatched;
}

static void
dispatcher_list_add(DispatcherList *self, Dispatcher *dispatcher)
{
    self->dispatchers = g_slist_prepend(self->dispatchers, dispatcher);
}

static bool
dispatcher_list_remove(DispatcherList *self,
                       const char *interface,
                       const char *member,
                       void       *notify_cb)

{
    bool removed = false;

    for( GSList *item = self->dispatchers; item; item = g_slist_next(item)) {
        Dispatcher  *dispatcher = item->data;

        if( dispatcher->notify_cb != notify_cb )
            continue;

        if( g_strcmp0(dispatcher->member, member) )
            continue;

        if( g_strcmp0(dispatcher->interface, interface) )
            continue;

        /* Detach dispatcher from item and delete it */
        item->data = 0;
        dispatcher_delete(dispatcher);

        /* Detach item from list */
        self->dispatchers = g_slist_delete_link(self->dispatchers, item);

        removed = true;
        break;
    }

    return removed;
}

static DispatcherList *
dispatcher_list_new(void)
{
    DispatcherList *self = g_new(DispatcherList, 1);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    self->dispatchers = 0;

    return self;
}

static void
dispatcher_list_delete(DispatcherList *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

        g_slist_free_full(self->dispatchers, dispatcher_delete_cb),
            self->dispatchers = 0;

        g_free(self);
    }
}

/* ------------------------------------------------------------------------- *
 * Filter
 * ------------------------------------------------------------------------- */

static DBusHandlerResult
filter_handle_message_cb(DBusConnection *connection,
                         DBusMessage    *msg,
                         gpointer        filter)
{
    Filter            *self   = filter;
    DBusHandlerResult  result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if( self->handler_cb(connection, msg, self->context) )
    {
        /* It is ok to have multiple handlers for signals etc.
         * Only method calls should be marked as "handled" */
        if( dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL )
        {
            result = DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    return result;
}

static Filter *
filter_new(FilterMessageHandler handler_cb, void *context)
{
    Filter    *self = g_new(Filter, 1);
    bool       ack  = false;

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    self->connection = 0;
    self->context    = context;
    self->handler_cb = handler_cb;

    if( !(self->connection = dsme_dbus_get_connection(0)) )
        goto EXIT;

    if( !dbus_connection_add_filter(self->connection,
                                    filter_handle_message_cb,
                                    self, 0) )
        goto EXIT;

    ack = true;

EXIT:
    if( !ack ) {
        filter_delete(self), self = 0;
    }

    return self;
}

static void
filter_delete(Filter *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

        if( self->connection ) {
            dbus_connection_remove_filter(self->connection,
                                          filter_handle_message_cb,
                                          self);

            dbus_connection_unref(self->connection),
                self->connection = 0;
        }
        g_free(self);
    }
}

/* ------------------------------------------------------------------------- *
 * Service
 * ------------------------------------------------------------------------- */

static bool
service_has_methods(const Service *self)
{
    return !dispatcher_list_is_empty(self->methods);
}

static bool
service_filter_cb(DBusConnection *connection,
                  DBusMessage    *msg,
                  gpointer        service)
{
    Service  *self    = service;
    bool      handled = false;

    /* Check that it is method call message sent to this service */
    if( dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL )
        goto EXIT;

    const char *destination = dbus_message_get_destination(msg);
    if( !destination || g_strcmp0(destination, self->name) )
        goto EXIT;

    handled = dispatcher_list_dispatch(self->methods, connection, msg);

EXIT:
    return handled;
}

static void
service_bind(Service        *self,
             const char     *interface,
             const char     *member,
             DsmeDbusMethod  notify_cb)
{
    dispatcher_list_add(self->methods,
                        method_dispatcher_new(interface, member, notify_cb));
}

static void
service_unbind(Service        *self,
               const char     *interface,
               const char     *member,
               DsmeDbusMethod  notify_cb)
{
    dsme_log(LOG_DEBUG, "unbind method %s.%s", interface, member);

    dispatcher_list_remove(self->methods, interface, member, notify_cb);
}

static bool service_reserve_name(Service *self)
{
    DBusError err = DBUS_ERROR_INIT;

    if( self->reserved )
        goto EXIT;

    if( !self->filter )
        goto EXIT;

    if( !dsme_dbus_connection_is_open(self->filter->connection) )
        goto EXIT;

    int rc = dbus_bus_request_name(self->filter->connection, self->name, 0, &err);

    if( rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ) {
        dsme_log(LOG_DEBUG, "request_name(%s): %s: %s\n",
                 self->name, err.name, err.message);
        goto EXIT;
    }

    dsme_log(LOG_DEBUG, "name %s reserved", self->name);
    self->reserved = true;

EXIT:
    dbus_error_free(&err);

    return self->reserved;
}

static void service_release_name(Service *self)
{
    DBusError err = DBUS_ERROR_INIT;

    if( !self->reserved )
        goto EXIT;

    self->reserved = false;

    if( !self->filter )
        goto EXIT;

    if( !dsme_dbus_connection_is_open(self->filter->connection) )
        goto EXIT;

    int rc = dbus_bus_release_name(self->filter->connection,
                                   self->name, &err);

    if( rc != DBUS_RELEASE_NAME_REPLY_RELEASED ) {
        dsme_log(LOG_DEBUG, "release_name(%s): %s: %s\n",
                 self->name, err.name, err.message);
    }

    dsme_log(LOG_DEBUG, "name %s released", self->name);

EXIT:
    dbus_error_free(&err);
}

static Service *
service_new(const char *name)
{
    bool      success = false;
    Service  *self    = 0;

    self = g_new(Service, 1);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    self->name     = g_strdup(name);
    self->reserved = false;
    self->methods  = dispatcher_list_new();
    self->filter   = filter_new(service_filter_cb, self);

    if( !self->filter )
        goto EXIT;

    if( !service_reserve_name(self) )
        goto EXIT;

    success = true;

EXIT:
    if( !success ) {
        service_delete(self),
            self = 0;
    }

    return self;
}

static void
service_delete(Service *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

        service_release_name(self);

        filter_delete(self->filter),
            self->filter = 0;

        dispatcher_list_delete(self->methods),
            self->methods = 0;

        g_free(self->name),
            self->name = 0;

        g_free(self);
    }
}

static void
service_delete_cb(gpointer self)
{
    service_delete(self);
}

/* ------------------------------------------------------------------------- *
 * Server
 * ------------------------------------------------------------------------- */

static Server *the_server = 0;

static bool
server_bind(Server         *self,
            const char     *service_name,
            const char     *interface,
            const char     *member,
            DsmeDbusMethod  notify_cb)
{
    bool bound = false;

    Service *service = g_datalist_get_data(&self->services, service_name);

    if( !service ) {
        if( !(service = service_new(service_name)) )
            goto EXIT;

        g_datalist_set_data_full(&self->services, service_name,
                                 service,
                                 service_delete_cb);
    }

    service_bind(service, interface, member, notify_cb);
    bound = true;

EXIT:

    dsme_log(bound ? LOG_DEBUG : LOG_WARNING,
             "bind method %s.%s - %s", interface, member,
             bound ? "ack" : "NAK");

    return bound;
}

static void
server_unbind(Server         *self,
              const char     *service_name,
              const char     *interface,
              const char     *member,
              DsmeDbusMethod  notify_cb)
{
    Service *service = g_datalist_get_data(&self->services, service_name);

    if( !service )
        goto EXIT;

    service_unbind(service, interface, member, notify_cb);

    if( !service_has_methods(service) ) {
        g_datalist_remove_data(&self->services, service_name),
            service = 0;
    }

EXIT:
    return;
}

static Server *
server_new(void)
{
    Server *self = g_new(Server, 1);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    g_datalist_init(&self->services);

    return self;
}

static Server *
server_instance(void)
{
    if( !the_server ) {
        if( !dsme_dbus_is_enabled() )
            dsme_log(LOG_ERR, "server instantiation attempt from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        else
            the_server = server_new();
    }

    return the_server;
}

static void
server_delete(Server *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);
        g_datalist_clear(&self->services);
        g_free(self);
    }
}

/* ------------------------------------------------------------------------- *
 * Client
 * ------------------------------------------------------------------------- */

static Client *the_client = 0;

static gchar *
client_match(const char *interface, const char *member)
{
    return g_strdup_printf("type='signal', interface='%s', member='%s'",
                           interface, member);
}

static bool
client_filter_cb(DBusConnection *connection,
                 DBusMessage    *msg,
                 gpointer        client)
{
    Client *self    = client;
    bool    handled = false;

    if( dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL ) {
        handled = dispatcher_list_dispatch(self->handlers, connection, msg);
    }

    return handled;
}

static bool
client_bind(Client          *self,
            const char      *interface,
            const char      *member,
            DsmeDbusHandler handler)
{
    bool   bound = false;
    gchar *match = 0;

    if( !dsme_dbus_connection_is_open(self->filter->connection) )
        goto EXIT;

    if( !(match = client_match(interface, member)) )
        goto EXIT;

    dispatcher_list_add(self->handlers,
                        handler_dispatcher_new(interface, member, handler));

    dbus_bus_add_match(self->filter->connection, match, 0);

    bound = true;

EXIT:

    g_free(match);

    dsme_log(bound ? LOG_DEBUG : LOG_WARNING,
             "bind signal %s.%s - %s", interface, member,
             bound ? "ack" : "NAK");

    return bound;
}

static void
client_unbind(Client          *self,
              const char      *interface,
              const char      *member,
              DsmeDbusHandler  handler)
{
    gchar *match = 0;

    dsme_log(LOG_DEBUG, "unbind signal %s.%s", interface, member);

    if( !dispatcher_list_remove(self->handlers, interface, member, handler) )
        goto EXIT;

    if( !dsme_dbus_connection_is_open(self->filter->connection) )
        goto EXIT;

    if( !(match = client_match(interface, member)) )
        goto EXIT;

    dbus_bus_remove_match(self->filter->connection, match, 0);

EXIT:

    g_free(match);
}

static Client *
client_new(void)
{
    Client *self = g_new(Client, 1);

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

    self->handlers = dispatcher_list_new();
    self->filter   = filter_new(client_filter_cb, self);

    if( !self->filter ) {
        client_delete(self), self = 0;
    }

    return self;
}

static Client *
client_instance(void)
{
    if( !the_client ) {
        if( !dsme_dbus_is_enabled() )
            dsme_log(LOG_ERR, "client instantiation attempt from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        else
            the_client = client_new();
    }

    return the_client;
}

static void
client_delete(Client *self)
{
    if( self ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, self);

        filter_delete(self->filter),
            self->filter = 0;

        dispatcher_list_delete(self->handlers),
            self->handlers = 0;

        g_free(self);
    }
}

/* ------------------------------------------------------------------------- *
 * DSME_DBUS
 * ------------------------------------------------------------------------- */

static bool dsme_dbus_enabled = false;

static bool
dsme_dbus_is_enabled(void)
{
    return dsme_dbus_enabled;
}

static const char *
dsme_dbus_caller_name(void)
{
    const char     *name   = 0;
    const module_t *module = current_module();

    if( module )
        name = module_name(module);

    return name ?: "UNKNOWN";
}

static bool
dsme_dbus_connection_is_open(DBusConnection *con)
{
    return con && dbus_connection_get_is_connected(con);
}

static bool
dsme_dbus_bus_get_unix_process_id(DBusConnection *conn,
                                  const char     *name,
                                  pid_t          *pid)
{
    bool          ack = false;
    DBusMessage  *req = 0;
    DBusMessage  *rsp = 0;
    DBusError     err = DBUS_ERROR_INIT;
    dbus_uint32_t dta = 0;

    if( !dsme_dbus_connection_is_open(conn) )
        goto EXIT;

    req = dbus_message_new_method_call("org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "GetConnectionUnixProcessID");
    if( !req ) {
        dsme_log(LOG_DEBUG, "Unable to allocate new message");
        goto EXIT;
    }

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_STRING,
                                  &name,
                                  DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_DEBUG, "Unable to append arguments to message");
        goto EXIT;
    }

    // TODO: it is risky that we are blocking

    rsp = dbus_connection_send_with_reply_and_block(conn, req, -1, &err);
    if( !rsp ) {
        dsme_log(LOG_ERR, "Sending GetConnectionUnixProcessID failed: %s",
                 err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_UINT32,
                               &dta,
                               DBUS_TYPE_INVALID) ) {
        dsme_log(LOG_ERR, "Getting GetConnectionUnixProcessID args failed: %s",
                 err.message);
        goto EXIT;
    }

    ack = true, *pid = dta;

EXIT:

    if( req )
        dbus_message_unref(req);

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);

    return ack;
}

static const char *
dsme_dbus_get_type_name(int type)
{
    static const char *res = "UNKNOWN";
    switch( type )
    {
    case DBUS_TYPE_INVALID:     res = "INVALID";     break;
    case DBUS_TYPE_BYTE:        res = "BYTE";        break;
    case DBUS_TYPE_BOOLEAN:     res = "BOOLEAN";     break;
    case DBUS_TYPE_INT16:       res = "INT16";       break;
    case DBUS_TYPE_UINT16:      res = "UINT16";      break;
    case DBUS_TYPE_INT32:       res = "INT32";       break;
    case DBUS_TYPE_UINT32:      res = "UINT32";      break;
    case DBUS_TYPE_INT64:       res = "INT64";       break;
    case DBUS_TYPE_UINT64:      res = "UINT64";      break;
    case DBUS_TYPE_DOUBLE:      res = "DOUBLE";      break;
    case DBUS_TYPE_STRING:      res = "STRING";      break;
    case DBUS_TYPE_OBJECT_PATH: res = "OBJECT_PATH"; break;
    case DBUS_TYPE_SIGNATURE:   res = "SIGNATURE";   break;
    case DBUS_TYPE_UNIX_FD:     res = "UNIX_FD";     break;
    case DBUS_TYPE_ARRAY:       res = "ARRAY";       break;
    case DBUS_TYPE_VARIANT:     res = "VARIANT";     break;
    case DBUS_TYPE_STRUCT:      res = "STRUCT";      break;
    case DBUS_TYPE_DICT_ENTRY:  res = "DICT_ENTRY";  break;
    }
    return res;
}

static bool
dsme_dbus_check_arg_type(DBusMessageIter *iter, int want_type)
{
    int have_type = dbus_message_iter_get_arg_type(iter);

    if( have_type == want_type )
        return true;

    dsme_log(LOG_WARNING, "dbus message parsing failed: expected %s, got %s",
             dsme_dbus_get_type_name(want_type),
             dsme_dbus_get_type_name(have_type));
    return false;
}

/* ------------------------------------------------------------------------- *
 * cached connection
 * ------------------------------------------------------------------------- */

static DBusConnection *the_connection = 0;

static DBusHandlerResult
dsme_dbus_connection_filter_cb(DBusConnection *con, DBusMessage *msg, void *aptr)
{
    switch( dbus_message_get_type(msg) ) {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
        break;

    case DBUS_MESSAGE_TYPE_SIGNAL:
        if( dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected") ) {
            dsme_log(LOG_CRIT, "Disconnected from system bus; rebooting");

            /* create flag file to mark failure */
            FILE *fh = fopen(DBUS_FAILED_FILE, "w+");
            if( fh )
                fclose(fh);

            /* issue reboot request */
            DSM_MSGTYPE_REBOOT_REQ req = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);
            broadcast_internally(&req);
        }
        break;

    case DBUS_MESSAGE_TYPE_ERROR:
        /* Give visibility to error replies that
         * we did not want or that arrived after
         * waiting for them already timed out. */
        {
            DBusError err = DBUS_ERROR_INIT;
            if( dbus_set_error_from_message(&err, msg) ) {
                dsme_log(LOG_WARNING, "D-Bus: %s: %s",
                         err.name, err.message);
            }
            dbus_error_free(&err);
        }
        break;

    default:
        break;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusConnection *
dsme_dbus_try_to_connect(DBusError *err)
{
    if( !dsme_dbus_is_enabled() ) {
        dsme_log(LOG_ERR, "connect attempt from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        goto EXIT;
    }

    if( the_connection )
        goto EXIT;

    if( !(the_connection = dbus_bus_get(DBUS_BUS_SYSTEM, err)) )
        goto EXIT;

    dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, the_connection);

    /* Use custom disconnect handler instead of exiting */
    dbus_connection_add_filter(the_connection,
                               dsme_dbus_connection_filter_cb, 0, 0);
    dbus_connection_set_exit_on_disconnect(the_connection, FALSE);

    /* Attach to glib mainloop */
    dbus_connection_setup_with_g_main(the_connection, 0);

EXIT:
    // NOTE: returns null or new reference
    return the_connection ? dbus_connection_ref(the_connection) : 0;
}

static void
dsme_dbus_disconnect(void)
{
    if( the_connection ) {
        dsme_log(LOG_DEBUG, "%s - %p", __FUNCTION__, the_connection);

        dbus_connection_remove_filter(the_connection,
                                      dsme_dbus_connection_filter_cb, 0);

        dbus_connection_unref(the_connection),
            the_connection = 0;
    }
}

DBusConnection *
dsme_dbus_get_connection(DBusError *err)
{
    DBusConnection *con = 0;
    DBusError       tmp = DBUS_ERROR_INIT;

    if( !dsme_dbus_is_enabled() ) {
        dsme_log(LOG_ERR, "connect requst from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        goto EXIT;
    }

    if( !(con = dsme_dbus_try_to_connect(&tmp)) ) {
        if( err )
            dbus_move_error(&tmp, err);
        else
            dsme_log(LOG_DEBUG, "dbus_bus_get(): %s\n", tmp.message);
    }

EXIT:

    dbus_error_free(&tmp);

    // NOTE: returns null or new reference
    return con;
}

bool
dsme_dbus_is_available(void)
{
    bool            res = false;

    if( dsme_dbus_is_enabled() ) {
        DBusConnection *con = 0;
        if( (con = dsme_dbus_try_to_connect(0)) ) {
            dbus_connection_unref(con);
            res = true;
        }
    }

    return res;
}

/* ------------------------------------------------------------------------- *
 * method call handlers
 * ------------------------------------------------------------------------- */

void
dsme_dbus_bind_methods(bool                      *bound,
                       const dsme_dbus_binding_t *bindings,
                       const char                *service,
                       const char                *interface)
{
    if( !dsme_dbus_is_enabled() ) {
        dsme_log(LOG_ERR, "method bind attempt from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        goto EXIT;
    }

    if( !bindings )
        goto EXIT;

    if( *bound )
        goto EXIT;

    *bound = true;

    Server *server = server_instance();

    if( !server )
        goto EXIT;

    for( ; bindings->method; ++bindings ) {
        server_bind(server, service, interface, bindings->name,
                    bindings->method);
    }

EXIT:
    return;
}

void
dsme_dbus_unbind_methods(bool                      *bound,
                         const dsme_dbus_binding_t *bindings,
                         const char                *service,
                         const char                *interface)
{
    if( !bindings )
        goto EXIT;

    if( !*bound )
        goto EXIT;

    Server *server = server_instance();

    if( !server )
        goto EXIT;

    *bound = false;

    for( ; bindings->method; ++bindings ) {
        server_unbind(server, service, interface, bindings->name,
                      bindings->method);
    }

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * signal handlers
 * ------------------------------------------------------------------------- */

void
dsme_dbus_bind_signals(bool                             *bound,
                       const dsme_dbus_signal_binding_t *bindings)
{
    if( !dsme_dbus_is_enabled() ) {
        dsme_log(LOG_ERR, "signal bind attempt from %s while dbus functionality disabled",
                 dsme_dbus_caller_name());
        goto EXIT;
    }

    if( !bindings )
        goto EXIT;

    if( *bound )
        goto EXIT;

    Client *client = client_instance();

    if( !client )
        goto EXIT;

    *bound = true;

    for( ; bindings->handler; ++bindings ) {
        client_bind(client, bindings->interface, bindings->name, bindings->handler);
    }

EXIT:
    return;
}

void
dsme_dbus_unbind_signals(bool                             *bound,
                         const dsme_dbus_signal_binding_t *bindings)
{
    if( !bindings )
        goto EXIT;

    if( !*bound )
        goto EXIT;

    Client *client = client_instance();

    if( !client )
        goto EXIT;

    *bound = false;

    for( ; bindings->handler; ++bindings ) {
        client_unbind(client, bindings->interface, bindings->name, bindings->handler);
    }

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * start / stop
 * ------------------------------------------------------------------------- */

void
dsme_dbus_startup(void)
{
    dsme_log(LOG_DEBUG, "%s", __FUNCTION__);
    dsme_dbus_enabled = true;
}

void
dsme_dbus_cleanup(void)
{
    dsme_log(LOG_DEBUG, "%s", __FUNCTION__);

    /* Already existing system bus connection and instance
     * objects can be used for cleanup purposes, but reinstating
     * singletons / re-establishing system bus connection is not
     * allowed anymore.
     */
    dsme_dbus_enabled = false;

    /* Destroy singletons */
    server_delete(the_server),
        the_server = 0;

    client_delete(the_client),
        the_client = 0;

    /* Let go of the system bus connection ref */
    dsme_dbus_disconnect();
}
