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
#include "dbusproxy.h"

#include "../include/dsme/logging.h"
#include "../include/dsme/modules.h"
#include "../include/dsme/modulebase.h"
#include "../dsme/dsme-server.h"
#include <dsme/state.h>

#include <dbus/dbus.h>
#include <dbus-gmain/dbus-gmain.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DBUS_FAILED_FILE "/run/systemd/boot-status/dbus-failed"

/* HACK: make sure also unused code gets a compilation attempt */
//#define DEAD_CODE

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

typedef struct DsmeDbusInterface DsmeDbusInterface;
typedef struct DsmeDbusObject    DsmeDbusObject;
typedef struct DsmeDbusService   DsmeDbusService;
typedef struct DsmeDbusManager   DsmeDbusManager;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * Helpers
 * ------------------------------------------------------------------------- */

static gchar **keys_from(GHashTable *lut);
static void    keys_free(gchar **keys);

/* ------------------------------------------------------------------------- *
 * DsmeDbusMessage
 * ------------------------------------------------------------------------- */

static DBusMessageIter   *message_iter                      (const DsmeDbusMessage *self);
static void               message_init_read_iterator        (DsmeDbusMessage *self);
static void               message_init_append_iterator      (DsmeDbusMessage *self);
static void               message_ctor                      (DsmeDbusMessage *self, DBusConnection *con, DBusMessage *msg, bool append);
static void               message_dtor                      (DsmeDbusMessage *self);
static DsmeDbusMessage   *message_new                       (DBusConnection *con, DBusMessage *msg);
static void               message_delete                    (DsmeDbusMessage *self);
static void               message_send_and_delete           (DsmeDbusMessage *self);

DsmeDbusMessage          *dsme_dbus_reply_new               (const DsmeDbusMessage *request);
DsmeDbusMessage          *dsme_dbus_reply_error             (const DsmeDbusMessage *request, const char *error_name, const char *error_message );
DsmeDbusMessage          *dsme_dbus_signal_new              (const char *sender, const char *path, const char *interface, const char *name);
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
 * DsmeDbusInterface
 * ------------------------------------------------------------------------- */

static void                       interface_introspect (DsmeDbusInterface *self, FILE *file);
static DsmeDbusInterface         *interface_create     (DsmeDbusObject *object, const char *name);
static void                       interface_delete     (DsmeDbusInterface *self);
static void                       interface_delete_cb  (void *self);
static const char                *interface_name       (const DsmeDbusInterface *self);
#ifdef DEAD_CODE
static const DsmeDbusObject      *interface_object     (const DsmeDbusInterface *self);
#endif
#ifdef DEAD_CODE
static DBusConnection            *interface_connection (const DsmeDbusInterface *self);
#endif
static const dsme_dbus_binding_t *interface_get_members(const DsmeDbusInterface *self);
static void                       interface_set_members(DsmeDbusInterface *self, const dsme_dbus_binding_t *members);

/* ------------------------------------------------------------------------- *
 * DsmeDbusObject
 * ------------------------------------------------------------------------- */

static void                object_introspect        (DsmeDbusObject *self, FILE *file);
static DsmeDbusObject     *object_create            (DsmeDbusService *service, const char *path);
static void                object_delete            (DsmeDbusObject *self);
static void                object_delete_cb         (void *self);
#ifdef DEAD_CODE
static const char         *object_path              (const DsmeDbusObject *self);
#endif
#ifdef DEAD_CODE
static DsmeDbusService    *object_service           (const DsmeDbusObject *self);
static DBusConnection     *object_connection        (const DsmeDbusObject *self);
#endif
#ifdef DEAD_CODE
static gchar             **object_get_intrface_names(const DsmeDbusObject *self);
#endif
static DsmeDbusInterface  *object_get_interface     (const DsmeDbusObject *self, const char *interface_name);
static DsmeDbusInterface  *object_add_interface     (DsmeDbusObject *self, const char *interface_name);
static bool                object_rem_interface     (DsmeDbusObject *self, const char *interface_name);
static bool                object_has_interfaces    (const DsmeDbusObject *self);

/* ------------------------------------------------------------------------- *
 * DsmeDbusService
 * ------------------------------------------------------------------------- */

static DsmeDbusService  *service_create          (DsmeDbusManager *server, const char *name);
static void              service_delete          (DsmeDbusService *self);
static void              service_delete_cb       (void *self);
#ifdef DEAD_CODE
static const char       *service_name            (const DsmeDbusService *self);
#endif
static DsmeDbusManager  *service_manager         (const DsmeDbusService *self);
static DBusConnection   *service_connection      (const DsmeDbusService *self);
static gchar           **service_get_object_paths(const DsmeDbusService *self);
static DsmeDbusObject   *service_get_object      (const DsmeDbusService *self, const char *object_path);
static DsmeDbusObject   *service_add_object      (DsmeDbusService *self, const char *object_path);
static bool              service_rem_object      (DsmeDbusService *self, const char *object_path);
static bool              service_has_objects     (const DsmeDbusService *self);
static void              service_acquire_name    (DsmeDbusService *self);
static void              service_release_name    (DsmeDbusService *self);
static gchar           **service_get_children_of (DsmeDbusService *self, const char *parent_path);

/* ------------------------------------------------------------------------- *
 * DsmeDbusManager
 * ------------------------------------------------------------------------- */

static DBusMessage      *manager_handle_introspect    (DsmeDbusManager *self, DBusMessage *req);
static DBusHandlerResult manager_message_filter_cb    (DBusConnection *con, DBusMessage *msg, void *aptr);
static gchar            *manager_generate_rule        (const dsme_dbus_signal_binding_t *binding);
static void              manager_set_module           (DsmeDbusManager *self, const void *context, const module_t *module);
static module_t         *manager_get_module           (DsmeDbusManager *self, const void *context);
static DsmeDbusManager  *manager_create               (void);
static void              manager_delete               (DsmeDbusManager *self);
#ifdef DEAD_CODE
static void              manager_delete_cb            (void *self);
#endif
static bool              manager_connect              (DsmeDbusManager *self);
static void              manager_disconnect           (DsmeDbusManager *self);
static DBusConnection   *manager_connection           (const DsmeDbusManager *self);
#ifdef DEAD_CODE
static gchar           **manager_get_service_names    (const DsmeDbusManager *self);
#endif
static DsmeDbusService  *manager_get_service          (const DsmeDbusManager *self, const char *service_name);
static DsmeDbusService  *manager_add_service          (DsmeDbusManager *self, const char *service_name);
static bool              manager_rem_service          (DsmeDbusManager *self, const char *service_name);
#ifdef DEAD_CODE
static bool              manager_has_services         (const DsmeDbusManager *self);
#endif
static void              manager_add_matches_one      (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *binding);
static void              manager_add_matches_array    (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings);
static void              manager_add_matches_all      (DsmeDbusManager *self);
static void              manager_rem_matches_one      (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *binding);
static void              manager_rem_matches_array    (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings);
static void              manager_rem_matches_all      (DsmeDbusManager *self);
static void              manager_add_handlers_array   (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings);
static void              manager_rem_handlers_array   (DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings);
static void              manager_rem_handlers_all     (DsmeDbusManager *self);
static void              manager_acquire_service_names(DsmeDbusManager *self);
static void              manager_release_service_names(DsmeDbusManager *self);
static bool              manager_verify_signal        (DsmeDbusManager *self, DBusConnection *con, DBusMessage *sig);
static bool              manager_handle_method        (DsmeDbusManager *self, DBusMessage *req);
static void              manager_handle_signal        (DsmeDbusManager *self, DBusMessage *sig);

/* ------------------------------------------------------------------------- *
 * Module
 * ------------------------------------------------------------------------- */

static void               dsme_dbus_verify_signal           (DBusConnection *con, DBusMessage *sig);
static bool               dsme_dbus_is_enabled              (void);
static const char        *dsme_dbus_calling_module_name     (void);
static bool               dsme_dbus_connection_is_open      (DBusConnection *con);
static bool               dsme_dbus_bus_get_unix_process_id (DBusConnection *conn, const char *name, pid_t *pid);
static const char        *dsme_dbus_get_type_name           (int type);
static bool               dsme_dbus_check_arg_type          (DBusMessageIter *iter, int want_type);
static const char        *dsme_dbus_name_request_reply_repr (int reply);
static const char        *dsme_dbus_name_release_reply_repr (int reply);

void                      dsme_dbus_bind_methods            (bool *bound, const char *service_name, const char *object_path, const char *interface_name, const dsme_dbus_binding_t *bindings);
void                      dsme_dbus_unbind_methods          (bool *bound, const char *service_name, const char *object_path, const char *interface_name, const dsme_dbus_binding_t *bindings);
void                      dsme_dbus_bind_signals            (bool *bound, const dsme_dbus_signal_binding_t *bindings);
void                      dsme_dbus_unbind_signals          (bool *bound, const dsme_dbus_signal_binding_t *bindings);

bool                      dsme_dbus_connect                 (void);
void                      dsme_dbus_disconnect              (void);
DBusConnection           *dsme_dbus_get_connection          (DBusError *err);

void                      dsme_dbus_startup                 (void);
void                      dsme_dbus_shutdown                (void);

/* ------------------------------------------------------------------------- *
 * Helpers
 * ------------------------------------------------------------------------- */

static gchar **
keys_from(GHashTable *lut)
{
    // Assumed: Keys are c-strings
    guint     size = 0;
    gpointer *data = 0;
    gchar   **keys = 0;
    size_t    used = 0;

    if( lut )
        data = g_hash_table_get_keys_as_array(lut, &size);

    keys = g_malloc(sizeof *keys * (size + 1));

    for( guint i = 0; i < size; ++i ) {
        if( data[i] )
            keys[used++] = g_strdup(data[i]);
    }
    keys[used] = 0;

    g_free(data);

    // Release with: g_strfreev()
    return keys;
}

static void
keys_free(gchar **keys)
{
    if( keys ) {
        for( size_t i = 0; keys[i]; ++i )
            g_free(keys[i]);
        g_free(keys);
    }
}

/* ------------------------------------------------------------------------- *
 * DsmeDbusMessage
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

static DBusMessageIter *
message_iter(const DsmeDbusMessage *self)
{
    /* The problem is that helper functions for parsing messages
     * claim to to take const DsmeDbusMessage pointer, while the
     * iterator data naturally needs to be modified...
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
        self = g_malloc0(sizeof *self);
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
            if( dbus_message_get_type(self->msg) == DBUS_MESSAGE_TYPE_SIGNAL )
                dsme_dbus_verify_signal(self->connection, self->msg);
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
dsme_dbus_signal_new(const char *sender,
                     const char *path,
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
                 interface, name, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( !(con = dsme_dbus_get_connection(0)) ) {
        dsme_log(LOG_ERR, "signal %s.%s send attempt from %s while not connected",
                 interface, name, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    msg = dbus_message_new_signal(path, interface, name);
    dbus_message_set_sender(msg, sender);
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

/* ========================================================================= *
 * DsmeDbusInterface
 * ========================================================================= */

struct DsmeDbusInterface
{
    /** Parent object */
    DsmeDbusObject            *if_object;

    /** Interface name */
    gchar                     *if_name;

    /** Member array */
    const dsme_dbus_binding_t *if_members;
};

static void interface_introspect(DsmeDbusInterface *self, FILE *file)
{
    fprintf(file, "<interface name=\"%s\">\n",
            interface_name(self));

    const dsme_dbus_binding_t *member = self->if_members;

    for( ; member && member->name; ++member ) {
        const char *type = member->method ? "method" : "signal";
        fprintf(file,"  <%s name=\"%s\">\n", type, member->name);
        if( member->args )
            fprintf(file, "%s", member->args);
        else
            fprintf(file, "    <!-- NOT DEFINED -->\n");
        fprintf(file, "  </%s>\n", type);
    }

    fprintf(file, "</interface>\n");
}

static DsmeDbusInterface *
interface_create(DsmeDbusObject *object, const char *name)
{
    DsmeDbusInterface *self = g_malloc0(sizeof *self);

    self->if_object  = object;
    self->if_name    = g_strdup(name);
    self->if_members = 0;

    return self;
}

static void
interface_delete(DsmeDbusInterface *self)
{
  if( self != 0 )
  {
      self->if_members = 0;
      self->if_object  = 0;

      g_free(self->if_name),
          self->if_name = 0;

      g_free(self);
  }
}

static void
interface_delete_cb(void *self)
{
  interface_delete(self);
}

static const char *
interface_name(const DsmeDbusInterface *self)
{
    return self->if_name;
}

#ifdef DEAD_CODE
static const DsmeDbusObject *
interface_object(const DsmeDbusInterface *self)
{
    return self->if_object;
}
#endif

#ifdef DEAD_CODE
static DBusConnection *
interface_connection(const DsmeDbusInterface *self)
{
    return object_connection(interface_object(self));
}
#endif

static const dsme_dbus_binding_t *
interface_get_members(const DsmeDbusInterface *self)
{
    return self->if_members;
}

static void
interface_set_members(DsmeDbusInterface *self, const dsme_dbus_binding_t *members)
{
    // set once
    if( self->if_members == 0 ) {
        self->if_members = members;
    }
    else if( self->if_members != members ) {
        dsme_log(LOG_CRIT, "TODO");
    }
}

/* ========================================================================= *
 * DsmeDbusObject
 * ========================================================================= */

struct DsmeDbusObject
{
    DsmeDbusService *ob_service;
    gchar           *ob_path;
    GHashTable      *ob_interfaces; // [name] -> DsmeDbusInterface *
};

static void object_introspect(DsmeDbusObject *self, FILE *file)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, self->ob_interfaces);
    while( g_hash_table_iter_next(&iter, &key, &value) )
    {
        DsmeDbusInterface *interface = value;
        interface_introspect(interface, file);
    }
}

static DsmeDbusObject *
object_create(DsmeDbusService *service, const char *path)
{
    DsmeDbusObject *self = g_malloc0(sizeof *self);

    self->ob_service    = service;
    self->ob_path       = g_strdup(path);
    self->ob_interfaces = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, interface_delete_cb);
    return self;
}

static void
object_delete(DsmeDbusObject *self)
{
    if( self ) {
        self->ob_service = 0;
        g_hash_table_unref(self->ob_interfaces),
            self->ob_interfaces = 0;
        g_free(self->ob_path),
            self->ob_path = 0;
        g_free(self);
    }
}

static void
object_delete_cb(void *self)
{
    object_delete(self);
}

#ifdef DEAD_CODE
static const char *
object_path(const DsmeDbusObject *self)
{
    return self->ob_path;
}
#endif

#ifdef DEAD_CODE
static DsmeDbusService *
object_service(const DsmeDbusObject *self)
{
    return self->ob_service;
}
#endif

#ifdef DEAD_CODE
static DBusConnection *
object_connection(const DsmeDbusObject *self)
{
    return service_connection(object_service(self));
}
#endif

#ifdef DEAD_CODE
static gchar **
object_get_intrface_names(const DsmeDbusObject *self)
{
    return keys_from(self->ob_interfaces);
}
#endif

static DsmeDbusInterface *
object_get_interface(const DsmeDbusObject *self, const char *interface_name)
{
    DsmeDbusInterface *interface = g_hash_table_lookup(self->ob_interfaces,
                                                       interface_name);
    return interface;
}

static DsmeDbusInterface *
object_add_interface(DsmeDbusObject *self, const char *interface_name)
{
    DsmeDbusInterface *interface = g_hash_table_lookup(self->ob_interfaces,
                                                       interface_name);
    if( !interface ) {
        interface = interface_create(self, interface_name);
        g_hash_table_replace(self->ob_interfaces,
                             g_strdup(interface_name),
                             interface);
    }

    return interface;
}

static bool
object_rem_interface(DsmeDbusObject *self, const char *interface_name)
{
    return g_hash_table_remove(self->ob_interfaces, interface_name);
}

static bool
object_has_interfaces(const DsmeDbusObject *self)
{
    return g_hash_table_size(self->ob_interfaces) > 0;
}

/* ========================================================================= *
 * DsmeDbusService
 * ========================================================================= */

struct DsmeDbusService
{
    DsmeDbusManager *se_manager;
    gchar           *se_name;
    GHashTable      *se_objects; // [name] -> DsmeDbusObject *

    bool             se_requested;
    bool             se_acquired;
};

static DsmeDbusService *
service_create(DsmeDbusManager *server, const char *name)
{
    DsmeDbusService *self = g_malloc0(sizeof *self);

    self->se_acquired  = false;
    self->se_requested = false;

    self->se_manager   = server;
    self->se_name      = g_strdup(name);
    self->se_objects   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, object_delete_cb);

    service_acquire_name(self);

    return self;
}

static void
service_delete(DsmeDbusService *self)
{
    if( self ) {
        service_release_name(self);

        self->se_manager = 0;

        g_hash_table_unref(self->se_objects),
            self->se_objects = 0;
        g_free(self->se_name),
            self->se_name = 0;
        g_free(self);
    }
}

static void
service_delete_cb(void *self)
{
    service_delete(self);
}

#ifdef DEAD_CODE
static const char *
service_name(const DsmeDbusService *self)
{
    return self->se_name;
}
#endif

static DsmeDbusManager *
service_manager(const DsmeDbusService *self)
{
    return self->se_manager;
}

static DBusConnection *
service_connection(const DsmeDbusService *self)
{
    return manager_connection(service_manager(self));
}

static gchar **
service_get_object_paths(const DsmeDbusService *self)
{
    return keys_from(self->se_objects);
}

static DsmeDbusObject *
service_get_object(const DsmeDbusService *self, const char *object_path)
{
    DsmeDbusObject *object = g_hash_table_lookup(self->se_objects,
                                                 object_path);
    return object;
}

static DsmeDbusObject *
service_add_object(DsmeDbusService *self, const char *object_path)
{
    DsmeDbusObject *object = g_hash_table_lookup(self->se_objects,
                                                 object_path);
    if( !object ) {
        object = object_create(self, object_path);
        g_hash_table_replace(self->se_objects,
                             g_strdup(object_path),
                             object);
    }

    return object;
}

static bool
service_rem_object(DsmeDbusService *self, const char *object_path)
{
    return g_hash_table_remove(self->se_objects, object_path);
}

static bool
service_has_objects(const DsmeDbusService *self)
{
    return g_hash_table_size(self->se_objects) > 0;
}

static void
service_acquire_name(DsmeDbusService *self)
{
    DBusError err = DBUS_ERROR_INIT;

    DBusConnection *connection = service_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    if( self->se_requested )
        goto EXIT;

    self->se_requested = true;

    int rc = dbus_bus_request_name(connection,
                                   self->se_name,
                                   DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                   &err);

    if( rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ) {
        if( dbus_error_is_set(&err) )
            dsme_log(LOG_ERR, "request_name(%s): %s: %s",
                     self->se_name, err.name, err.message);
        else
            dsme_log(LOG_ERR, "request_name(%s): %s",
                     self->se_name, dsme_dbus_name_request_reply_repr(rc));
        goto EXIT;
    }

    dsme_log(LOG_DEBUG, "name %s reserved", self->se_name);
    self->se_acquired = true;

EXIT:

    dbus_error_free(&err);

    return;
}

static void
service_release_name(DsmeDbusService *self)
{
    DBusError err = DBUS_ERROR_INIT;

    DBusConnection *connection = service_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    if( !self->se_acquired )
        goto EXIT;

    int rc = dbus_bus_release_name(connection,
                                   self->se_name, &err);

    if( rc != DBUS_RELEASE_NAME_REPLY_RELEASED ) {
        if( dbus_error_is_set(&err) )
            dsme_log(LOG_ERR, "release_name(%s): %s: %s",
                     self->se_name, err.name, err.message);
        else
            dsme_log(LOG_ERR, "release_name(%s): %s",
                     self->se_name, dsme_dbus_name_release_reply_repr(rc));
    }

    dsme_log(LOG_DEBUG, "name %s released", self->se_name);

EXIT:

    self->se_acquired  = false;
    self->se_requested = false;

    dbus_error_free(&err);

    return;
}

static gchar **
service_get_children_of(DsmeDbusService *self, const char *parent_path)
{
    size_t      parent_len = strlen(parent_path);
    GHashTable *child_lut  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);
    gchar     **object_vec = service_get_object_paths(self);

    for( size_t i = 0; object_vec[i]; ++i ) {
        if( strncmp(object_vec[i], parent_path, parent_len) )
            continue;

        const char *beg = object_vec[i] + parent_len;
        while( *beg == '/' )
            ++beg;

        const char *end = beg;
        while( *end && *end != '/' )
            ++end;

        if( end > beg ) {
            gchar *child = g_strndup(beg, end-beg);
            g_hash_table_replace(child_lut, child, 0);
        }
    }

    gchar **child_vec = keys_from(child_lut);

    keys_free(object_vec);
    g_hash_table_unref(child_lut);

    return child_vec;
}

/* ========================================================================= *
 * DsmeDbusManager
 * ========================================================================= */

struct DsmeDbusManager
{
    DBusConnection *mr_connection;
    GHashTable     *mr_services;  // [name] -> DsmeDbusService *
    GSList         *mr_handlers;  // iterm->data -> dsme_dbus_signal_binding_t array
    GHashTable     *mr_matches;   // [DsmeDbusService *] -> match string
    GHashTable     *mr_modules;   // void * -> module_t
};

/** Format string for Introspect XML prologue */
static const char INTROSPECT_PROLOG[] = ""
"<!DOCTYPE node PUBLIC"
" \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node name=\"%s\">\n";

/** Format string for Introspect XML epilogue */
static const char INTROSPECT_EPILOG[] = ""
"</node>\n";

/** Standard Introspectable interface */
static const char INTROSPECT_INTROSPECTABLE[] = ""
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\">\n"
"      <arg direction=\"out\" name=\"data\" type=\"s\"/>\n"
"    </method>\n"
"  </interface>\n";

/** Standard Peer interface */
static const char INTROSPECT_PEER[] = ""
"  <interface name=\"org.freedesktop.DBus.Peer\">\n"
"    <method name=\"Ping\"/>\n"
"    <method name=\"GetMachineId\">\n"
"      <arg direction=\"out\" name=\"machine_uuid\" type=\"s\" />\n"
"    </method>\n"
"  </interface>\n";

/** D-Bus callback for org.freedesktop.DBus.Introspectable.Introspect
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE
 */
static DBusMessage *
manager_handle_introspect(DsmeDbusManager *self, DBusMessage *req)
{
    DBusMessage *rsp  = NULL;
    FILE        *file = 0;
    char        *data = 0;
    size_t       size = 0;

    const char *service_name = dbus_message_get_destination(req);
    const char *object_path = dbus_message_get_path(req);

    dsme_log(LOG_WARNING, "Received introspect request: %s %s",
             service_name, object_path);

    DsmeDbusService *service = manager_get_service(self, service_name);
    if( !service )
        goto EXIT;

    if( !object_path ) {
        /* Should not really be possible, but ... */
        rsp = dbus_message_new_error(req, DBUS_ERROR_INVALID_ARGS,
                                     "object path not specified");
        goto EXIT;
    }

    DsmeDbusObject *object = service_get_object(service, object_path);
    gchar **children = service_get_children_of(service, object_path);

    if( !object && !*children ) {
        rsp = dbus_message_new_error_printf(req, DBUS_ERROR_UNKNOWN_OBJECT,
                                            "%s is not a valid object path",
                                            object_path);
        goto EXIT;
    }

    if( !(file = open_memstream(&data, &size)) )
        goto EXIT;

    fprintf(file, INTROSPECT_PROLOG, service_name);
    fprintf(file, INTROSPECT_INTROSPECTABLE);
    fprintf(file, INTROSPECT_PEER);

    if( object )
        object_introspect(object, file);

    for( size_t i = 0; children[i]; ++i )
        fprintf(file, "  <node name=\"%s\"/>\n", children[i]);

    fprintf(file, INTROSPECT_EPILOG);

    // the 'data' pointer gets updated at fclose
    fclose(file), file = 0;

    if( !data ) {
        rsp = dbus_message_new_error(req, DBUS_ERROR_FAILED,
                                     "failed to generate introspect xml data");
        goto EXIT;
    }

    /* Create a reply */
    rsp = dbus_message_new_method_return(req);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &data,
                                  DBUS_TYPE_INVALID) ) {
        dsme_log(LOG_ERR, "Failed to append reply argument to D-Bus"
                 " message for %s.%s",
                 DBUS_INTERFACE_INTROSPECTABLE,
                 "Introspect");
    }

EXIT:
    if( file )
        fclose(file);
    free(data);

    return rsp;
}

static DBusHandlerResult
manager_message_filter_cb(DBusConnection *con, DBusMessage *msg, void *aptr)
{
    DsmeDbusManager *self = aptr;

    /* Dispatching context can/should be defined in methdo call and
     * signal handler configuration. If not, make sure we default to
     * "core" module context. */
    const module_t *caller = modulebase_enter_module(0);

    DBusHandlerResult result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    switch( dbus_message_get_type(msg) ) {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
        if( dbus_message_is_method_call(msg,
                                        "org.freedesktop.DBus.Introspectable",
                                        "Introspect") ) {

            DBusMessage *rsp = manager_handle_introspect(self, msg);
            if( rsp ) {
                dbus_connection_send(con, rsp, 0);
                dbus_message_unref(rsp);
                result = DBUS_HANDLER_RESULT_HANDLED;
            }
        }
        else {
            if( manager_handle_method(self, msg) )
                result = DBUS_HANDLER_RESULT_HANDLED;
        }
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
            modules_broadcast_internally(&req);
        }
        else {
            manager_handle_signal(self, msg);
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

    modulebase_enter_module(caller);
    return result;;
}

static gchar *
manager_generate_rule(const dsme_dbus_signal_binding_t *binding)
{
    gchar *interface = 0;
    gchar *member    = 0;

    if( binding->interface )
        interface = g_strdup_printf(",interface='%s'", binding->interface);

    if( binding->name )
        member = g_strdup_printf(",member='%s'", binding->name);

    gchar *rule = g_strdup_printf("type='signal'%s%s",
                                  interface ?: "",
                                  member    ?: "");

    g_free(member);
    g_free(interface);
    return rule;
}

static void
manager_set_module(DsmeDbusManager *self, const void *context, const module_t *module)
{
    if( module )
        g_hash_table_replace(self->mr_modules, (void *)context, (void *)module);
    else
        g_hash_table_remove(self->mr_modules, context);
}

static module_t *
manager_get_module(DsmeDbusManager *self, const void *context)
{
    return g_hash_table_lookup(self->mr_modules, context);
}

static DsmeDbusManager *
manager_create(void)
{
    DsmeDbusManager *self = g_malloc0(sizeof *self);

    self->mr_handlers = 0;
    self->mr_services = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, service_delete_cb);

    self->mr_matches = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             0, g_free);

    self->mr_modules = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             0, 0);

    return self;
}

static void
manager_delete(DsmeDbusManager *self)
{
    if( self ) {

        manager_disconnect(self);

        manager_rem_handlers_all(self);

        g_hash_table_unref(self->mr_services),
            self->mr_services = 0;

        g_hash_table_unref(self->mr_matches),
            self->mr_matches = 0;

        g_hash_table_unref(self->mr_modules),
            self->mr_modules = 0;

        g_free(self);
    }
}

#ifdef DEAD_CODE
static void
manager_delete_cb(void *self)
{
    manager_delete(self);
}
#endif

static bool
manager_connect(DsmeDbusManager *self)
{
    DBusError       err = DBUS_ERROR_INIT;
    DBusConnection *con = 0;

    if( self->mr_connection )
        goto EXIT;

    if( !(con = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err)) ) {
        dsme_log(LOG_ERR, "system bus connect failed: %s: %s",
                 err.name, err.message);
        goto EXIT;
    }

    dsme_log(LOG_DEBUG, "connected to system bus");

    /* Set up message handler for the connection */
    dbus_connection_add_filter(con, manager_message_filter_cb, self, 0);

    /* Connection handler will deal with disconnect signals */
    dbus_connection_set_exit_on_disconnect(con, FALSE);

    /* Attach to glib mainloop */
    dbus_gmain_set_up_connection(con, 0);

    /* Manager owns the connection */
    self->mr_connection = con, con = 0;

    /* Add signal match rules */
    manager_add_matches_all(self);

    /* Acquire service names */
    manager_acquire_service_names(self);

EXIT:
    if( con )
        dbus_connection_unref(con);

    dbus_error_free(&err);

    /* Return value is: We are connected */
    return self->mr_connection != 0;
}

static void
manager_disconnect(DsmeDbusManager *self)
{
    /* Already disconnected? */
    if( !self->mr_connection )
        goto EXIT;

    /* Remove message handler */
    dbus_connection_remove_filter(self->mr_connection,
                                  manager_message_filter_cb,
                                  self);

    /* Remove active signal match rules */
    manager_rem_matches_all(self);

    /* Release acquired service names */
    manager_release_service_names(self);

    /* Close the socket */
    dbus_connection_close(self->mr_connection);

    /* Let go of the connection */
    dbus_connection_unref(self->mr_connection),
        self->mr_connection = 0;

    dsme_log(LOG_DEBUG, "disconnected from system bus");

EXIT:

    return;
}

static DBusConnection *
manager_connection(const DsmeDbusManager *self)
{
    return self->mr_connection;
}

#ifdef DEAD_CODE
static gchar **
manager_get_service_names(const DsmeDbusManager *self)
{
    return keys_from(self->mr_services);
}
#endif

static DsmeDbusService *
manager_get_service(const DsmeDbusManager *self, const char *service_name)
{
    DsmeDbusService *service = g_hash_table_lookup(self->mr_services,
                                                   service_name);
    return service;
}

static DsmeDbusService *
manager_add_service(DsmeDbusManager *self, const char *service_name)
{
    DsmeDbusService *service = g_hash_table_lookup(self->mr_services,
                                                   service_name);
    if( !service ) {
        service = service_create(self, service_name);
        g_hash_table_replace(self->mr_services,
                             g_strdup(service_name),
                             service);
    }

    return service;
}

static bool
manager_rem_service(DsmeDbusManager *self, const char *service_name)
{
    return g_hash_table_remove(self->mr_services, service_name);
}

#ifdef DEAD_CODE
static bool
manager_has_services(const DsmeDbusManager *self)
{
    return g_hash_table_size(self->mr_services) > 0;
}
#endif

static void
manager_add_matches_one(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *binding)
{
    DBusConnection *connection = manager_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    if( g_hash_table_lookup(self->mr_matches, binding) )
        goto EXIT;

    gchar *rule = manager_generate_rule(binding);
    dsme_log(LOG_DEBUG, "add match: %s", rule);
    dbus_bus_add_match(connection, rule, 0);
    g_hash_table_replace(self->mr_matches, (void*)binding, rule);

EXIT:
    return;
}

static void
manager_add_matches_array(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings)
{
    for( ; bindings->name; ++bindings )
        manager_add_matches_one(self, bindings);
}

static void
manager_add_matches_all(DsmeDbusManager *self)
{
    for( GSList *item = self->mr_handlers; item; item = item->next ) {
        const dsme_dbus_signal_binding_t *bindings = item->data;
        if( bindings )
            manager_add_matches_array(self, bindings);
    }
}

static void
manager_rem_matches_one(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *binding)
{
    const gchar *rule = g_hash_table_lookup(self->mr_matches, binding);
    if( !rule )
        goto EXIT;

    dsme_log(LOG_DEBUG, "remove match: %s", rule);

    DBusConnection *connection = manager_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    dbus_bus_remove_match(connection, rule, 0);

EXIT:
    if( rule )
        g_hash_table_remove(self->mr_matches, binding);

    return;
}

static void
manager_rem_matches_array(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings)
{
    for( ; bindings->name; ++bindings )
        manager_rem_matches_one(self, bindings);
}

static void
manager_rem_matches_all(DsmeDbusManager *self)
{
    for( GSList *item = self->mr_handlers; item; item = item->next ) {
        const dsme_dbus_signal_binding_t *bindings = item->data;
        if( bindings )
            manager_rem_matches_array(self, bindings);
    }
}

static void
manager_add_handlers_array(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings)
{
    if( !g_slist_find(self->mr_handlers, bindings) ) {
        self->mr_handlers = g_slist_append(self->mr_handlers, (void *)bindings);
        manager_add_matches_array(self, bindings);
    }
}

static void
manager_rem_handlers_array(DsmeDbusManager *self, const dsme_dbus_signal_binding_t *bindings)
{
    GSList *item = g_slist_find(self->mr_handlers, bindings);
    if( item ) {
        item->data = 0;
        self->mr_handlers = g_slist_delete_link(self->mr_handlers, item);
        manager_rem_matches_array(self, bindings);
    }
}
static void
manager_rem_handlers_all(DsmeDbusManager *self)
{
    while( self->mr_handlers )
        manager_rem_handlers_array(self, self->mr_handlers->data);
}

static void
manager_acquire_service_names(DsmeDbusManager *self)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, self->mr_services);
    while( g_hash_table_iter_next(&iter, &key, &value) )
    {
        DsmeDbusService *service = value;
        service_acquire_name(service);
    }
}

static void
manager_release_service_names(DsmeDbusManager *self)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, self->mr_services);
    while( g_hash_table_iter_next (&iter, &key, &value) )
    {
        DsmeDbusService *service = value;
        service_release_name(service);
    }
}

/** Check if the signal is ok to broadcast
 *
 * @param self Manager object, or NULL
 * @param sig  Signal message to check
 *
 * @return true if there are no objections to sending the signal,
 *         or false otherwise
 */
static bool
manager_verify_signal(DsmeDbusManager *self, DBusConnection *con,
                      DBusMessage *sig)
{
    bool exists = false;

    const char *service_name   = dbus_message_get_sender(sig);
    const char *object_path    = dbus_message_get_path(sig);
    const char *interface_name = dbus_message_get_interface(sig);
    const char *member         = dbus_message_get_member(sig);

    /* Must be valid manager object */
    if( !self )
        goto EXIT;

    /* Must be connected */
    DBusConnection *connection = manager_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    /* Current connection must be targeted */
    if( connection != con )
        goto EXIT;

    /* The signal must me introspectable */
    DsmeDbusService *service = manager_get_service(self, service_name);
    if( !service )
        goto EXIT;

    DsmeDbusObject *object = service_get_object(service,object_path);
    if( !object )
        goto EXIT;

    DsmeDbusInterface *interface = object_get_interface(object, interface_name);
    if( !interface )
        goto EXIT;

    const dsme_dbus_binding_t *bindings = interface_get_members(interface);
    if( !bindings )
        goto EXIT;

    if( !member )
        goto EXIT;

    for( ; bindings->name; ++bindings ) {
        if( bindings->method )
            continue;

        if( strcmp(bindings->name, member) )
            continue;

        exists = true;
        break;
    }

EXIT:
    if( !exists ) {
        dsme_log(LOG_WARNING, "failed to verify signal: %s %s %s.%s()",
                 service_name, object_path, interface_name, member);
    }

    return exists;
}

static bool
manager_handle_method(DsmeDbusManager *self, DBusMessage *req)
{
    bool handled = false;

    const char *service_name = dbus_message_get_destination(req);
    const char *object_path = dbus_message_get_path(req);
    const char *interface_name = dbus_message_get_interface(req);
    const char *member = dbus_message_get_member(req);

    DBusConnection *connection = manager_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    DsmeDbusService *service = manager_get_service(self, service_name);
    if( !service )
        goto EXIT;

    DsmeDbusObject *object = service_get_object(service,object_path);
    if( !object )
        goto EXIT;

    DsmeDbusInterface *interface = object_get_interface(object, interface_name);
    if( !interface )
        goto EXIT;

    const dsme_dbus_binding_t *bindings = interface_get_members(interface);
    if( !bindings )
        goto EXIT;

    if( !member )
        goto EXIT;

    module_t *module = manager_get_module(self, bindings);

    for( ; bindings->name; ++bindings ) {
        if( !bindings->method )
            continue;

        if( strcmp(bindings->name, member) )
            continue;

        DsmeDbusMessage *reply  = 0;

        DsmeDbusMessage  message;
        message_ctor(&message, connection, req, false);

        const module_t *restore = modulebase_current_module();

        dsme_log(LOG_DEBUG, "dispatch method %s.%s @ %s",
                 interface_name, member,
                 module ? module_name(module) : "(current");

        if( module )
            modulebase_enter_module(module);
        bindings->method(&message, &reply);
        modulebase_enter_module(restore);

        if( !dbus_message_get_no_reply(req) ) {
            if( !reply ) {
                dsme_log(LOG_WARNING, "dummy reply to %s.%s",
                         interface_name, member);
                reply = dsme_dbus_reply_error(&message,
                                              DBUS_ERROR_FAILED,
                                              "no reply to send");
            }

            if( reply && reply != DSME_DBUS_MESSAGE_DUMMY ) {
                message_send_and_delete(reply), reply = 0;
            }
        }
        else if( reply ) {
                dsme_log(LOG_WARNING, "discarding reply to %s.%s",
                         interface_name, member);
        }
        if( reply != DSME_DBUS_MESSAGE_DUMMY )
            message_delete(reply);
        message_dtor(&message);

        handled = true;
        break;
    }

EXIT:
    if( !handled ) {
        dsme_log(LOG_WARNING, "failed to dispatch method: %s %s %s.%s()",
                 service_name, object_path, interface_name, member);
    }

    return handled;
}

static void
manager_handle_signal(DsmeDbusManager *self, DBusMessage *sig)
{
    DBusConnection *connection = manager_connection(self);
    if( !dsme_dbus_connection_is_open(connection) )
        goto EXIT;

    const char *interface = dbus_message_get_interface(sig);
    if( !interface )
        goto EXIT;

    const char *member = dbus_message_get_member(sig);
    if( !member )
        goto EXIT;

    for( GSList *item = self->mr_handlers; item; item = item->next ) {
        const dsme_dbus_signal_binding_t *bindings = item->data;
        if( !bindings )
            continue;

        module_t *module = manager_get_module(self, bindings);

        for( ; bindings->name; ++bindings ) {
            if( strcmp(bindings->name, member) )
                continue;

            if( strcmp(bindings->interface, interface) )
                continue;

            DsmeDbusMessage  message;
            message_ctor(&message, connection, sig, false);

            const module_t *restore = modulebase_current_module();

            dsme_log(LOG_DEBUG, "dispatch signal %s.%s @ %s",
                     interface, member,
                     module ? module_name(module) : "(current");

            if( module )
                modulebase_enter_module(module);
            bindings->handler(&message);
            modulebase_enter_module(restore);

            message_dtor(&message);
        }
    }

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * DSME_DBUS
 * ------------------------------------------------------------------------- */

static const char *
dsme_dbus_calling_module_name(void)
{
    return module_name(modulebase_current_module()) ?: "UNKNOWN";
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
        dsme_log(LOG_ERR, "Unable to allocate new message");
        goto EXIT;
    }

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_STRING,
                                  &name,
                                  DBUS_TYPE_INVALID) )
    {
        dsme_log(LOG_ERR, "Unable to append arguments to message");
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

static const char *
dsme_dbus_name_request_reply_repr(int reply)
{
    const char *repr = "UNKNOWN";

    switch( reply ) {
    case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER:
        repr = "PRIMARY_OWNER";
        break;
    case DBUS_REQUEST_NAME_REPLY_IN_QUEUE:
        repr = "IN_QUEUE";
        break;
    case DBUS_REQUEST_NAME_REPLY_EXISTS:
        repr = "EXISTS";
        break;
    case DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER:
        repr = "ALREADY_OWNER";
        break;
    default:
        break;
    }

    return repr;
}

static const char *
dsme_dbus_name_release_reply_repr(int reply)
{
    const char *repr = "UNKNOWN";

    switch( reply ) {
    case DBUS_RELEASE_NAME_REPLY_RELEASED:
        repr = "RELEASED";
        break;
    case DBUS_RELEASE_NAME_REPLY_NON_EXISTENT:
        repr = "NON_EXISTENT";
        break;
    case DBUS_RELEASE_NAME_REPLY_NOT_OWNER:
        repr = "NOT_OWNER";
        break;
    default:
        break;
    }

    return repr;
}

/* ========================================================================= *
 * Module
 * ========================================================================= */

/** D-Bus manager singleton */
static DsmeDbusManager *the_manager = 0;

static bool
dsme_dbus_is_enabled(void)
{
    return the_manager != 0;
}

/** Warn when about to broadcast suspicious signal messages
 */
static void
dsme_dbus_verify_signal(DBusConnection *con, DBusMessage *sig)
{
    /* Note: Null manager check omitted on purpose */
    manager_verify_signal(the_manager, con, sig);
}

/* ------------------------------------------------------------------------- *
 * service & interface management
 * ------------------------------------------------------------------------- */

void
dsme_dbus_bind_methods(bool                      *bound,
                       const char                *service_name,
                       const char                *object_path,
                       const char                *interface_name,
                       const dsme_dbus_binding_t *bindings)
{
    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( *bound )
        goto EXIT;

    *bound = true;

    if( !bindings )
        goto EXIT;

    dsme_log(LOG_DEBUG, "binding interface %s", interface_name);

    /* Construct hierarchy as needed
     */
    DsmeDbusService   *service   = manager_add_service(the_manager, service_name);
    DsmeDbusObject    *object    = service_add_object(service,object_path);
    DsmeDbusInterface *interface = object_add_interface(object, interface_name);

    /* Bind methods to interface
     */
    manager_set_module(the_manager, bindings, modulebase_current_module());
    interface_set_members(interface, bindings);

EXIT:
    return;
}

void
dsme_dbus_unbind_methods(bool                      *bound,
                         const char                *service_name,
                         const char                *object_path,
                         const char                *interface_name,
                         const dsme_dbus_binding_t *bindings)
{
    if( !*bound )
        goto EXIT;

    *bound = false;

    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( !bindings )
        goto EXIT;

    dsme_log(LOG_DEBUG, "unbinding interface %s", interface_name);

    /* Check if the given interface is bound
     */
    DsmeDbusService *service = manager_get_service(the_manager, service_name);
    if( !service )
        goto EXIT;

    DsmeDbusObject *object = service_get_object(service,object_path);
    if( !object )
        goto EXIT;

    DsmeDbusInterface *interface = object_get_interface(object, interface_name);
    if( !interface )
        goto EXIT;

    if( interface_get_members(interface) != bindings )
        goto EXIT;

    /* Remove the interface
     */

    manager_set_module(the_manager, bindings, 0);

    if( !object_rem_interface(object, interface_name) )
        goto EXIT;

    /* Remove the parent object if it is no longer needed
     */
    if( object_has_interfaces(object) )
        goto EXIT;
    if( !service_rem_object(service, object_path) )
        goto EXIT;

    /* Remove the parent service if it is no longer needed
     */
    if( service_has_objects(service) )
        goto EXIT;

    manager_rem_service(the_manager, service_name);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * signal handler management
 * ------------------------------------------------------------------------- */

void
dsme_dbus_bind_signals(bool                             *bound,
                       const dsme_dbus_signal_binding_t *bindings)
{
    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( *bound )
        goto EXIT;

    *bound = true;

    if( !bindings )
        goto EXIT;

    dsme_log(LOG_DEBUG, "binding handlers for interface:  %s", bindings->interface);

    manager_set_module(the_manager, bindings, modulebase_current_module());
    manager_add_handlers_array(the_manager, bindings);

EXIT:
    return;
}

void
dsme_dbus_unbind_signals(bool                             *bound,
                         const dsme_dbus_signal_binding_t *bindings)
{
    if( !*bound )
        goto EXIT;

    *bound = false;

    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( !bindings )
        goto EXIT;

    dsme_log(LOG_DEBUG, "unbinding handlers for interface: %s", bindings->interface);

    manager_set_module(the_manager, bindings, 0);
    manager_rem_handlers_array(the_manager, bindings);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * system bus connection management
 * ------------------------------------------------------------------------- */

/** Connect to SystemBus
 *
 * Should be called only from dbusautoconnector plugin, or from dbusproxy
 * plugin as a response to DSM_MSGTYPE_DBUS_CONNECTED request.
 *
 * @return true if successfully connected, false otherwise
 */
bool
dsme_dbus_connect(void)
{
    bool connected = false;

    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( (connected = manager_connect(the_manager)) ) {
        DSM_MSGTYPE_DBUS_CONNECTED msg = DSME_MSG_INIT(DSM_MSGTYPE_DBUS_CONNECTED);
        modules_broadcast_internally(&msg);
    }

EXIT:
    return connected;
}

/** Disconnect from SystemBus
 *
 * Should be called only from dbusproxy plugin as a response to
 * DSM_MSGTYPE_DBUS_DISCONNECT request.
 */
void
dsme_dbus_disconnect(void)
{
    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    manager_disconnect(the_manager);

EXIT:
    return;
}

/** Get the current SystemBus connection
 *
 * Can be called from anywhere as long as dbus functionality
 * is in enabled state.
 *
 * Note: If non-null connection is returned, the caller must
 *       release if via dbus_connection_unref().
 *
 * @return fresh connection reference, or NULL
 */
DBusConnection *
dsme_dbus_get_connection(DBusError *err)
{
    DBusConnection *con = 0;

    if( !the_manager ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    con = manager_connection(the_manager);

EXIT:
    // FIXME: remove the err parameter altogether
    if( !con && err ) {
        dbus_set_error(err, DBUS_ERROR_DISCONNECTED,
                       "dsme is not connected to system bus");
    }

    // NOTE: returns null or new reference
    return con ? dbus_connection_ref(con) : 0;
}

/* ------------------------------------------------------------------------- *
 * start / stop dbus functionality
 * ------------------------------------------------------------------------- */

static bool dsme_dbus_terminated = false;
static bool dsme_dbus_initialized = false;

/** Enable dsme dbus functionality
 *
 * Should be called only on dbusproxy plugin load
 */
void
dsme_dbus_startup(void)
{
    if( dsme_dbus_terminated ) {
        dsme_log(LOG_ERR, "unallowable %s() call from %s",
                 __FUNCTION__, dsme_dbus_calling_module_name());
        goto EXIT;
    }

    if( dsme_dbus_initialized )
        goto EXIT;

    dsme_dbus_initialized = true;
    dsme_log(LOG_DEBUG, "dbus functionality enabled");

    /* Create D-Bus manager singleton */
    if( !the_manager )
        the_manager = manager_create();

EXIT:
    return;
}

/** Enable dsme dbus functionality
 *
 * Should be called only on dbusproxy plugin unload
 */
void
dsme_dbus_shutdown(void)
{
    if( dsme_dbus_terminated )
        goto EXIT;

    dsme_dbus_terminated = true;
    dsme_log(LOG_DEBUG, "dbus functionality disabled");

    /* Delete D-Bus manager singleton */
    manager_delete(the_manager), the_manager = 0;

    if( dsme_in_valgrind_mode() ) {
        /* Exchaust dbus message recycling cache */
        DBusMessage *msg[32];
        for( size_t i = 0; i < 32; ++i )
            msg[i] = dbus_message_new_signal("/", "foo.bar", "baf");
        for( size_t i = 0; i < 32; ++i )
            dbus_message_unref(msg[i]);
    }

EXIT:
    return;
}

#ifdef DEAD_CODE
// Silence warnings about functions that are known to be unused ...
void *dsme_dbus_unused_on_purpose[] =
{
    interface_connection,
    object_path,
    object_get_intrface_names,
    service_name,
    manager_delete_cb,
    manager_get_service_names,
    manager_has_services,
};
#endif
