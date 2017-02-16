/**
   @file dsme_dbus.h

   D-Bus C binding for DSME
   <p>
   Copyright (C) 2009-2010 Nokia Corporation.
   Copyright (C) 2013-2017 Jolla Ltd.

   @author Semi Malinen <semi.malinen@nokia.com>
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

#ifndef DSME_DBUS_H
#define DSME_DBUS_H

#include <stdbool.h>
#include <dbus/dbus.h>

#define DBUS_FAILED_FILE "/run/systemd/boot-status/dbus-failed"

typedef struct DsmeDbusMessage DsmeDbusMessage;

typedef void (*DsmeDbusMethod)(const DsmeDbusMessage* request,
                               DsmeDbusMessage**      reply);

typedef void (*DsmeDbusHandler)(const DsmeDbusMessage* ind);

typedef struct dsme_dbus_binding_t
{
    DsmeDbusMethod  method;
    const char     *name; // = member
} dsme_dbus_binding_t;

typedef struct dsme_dbus_signal_binding_t
{
    DsmeDbusHandler  handler;
    const char      *interface;
    const char      *name; // = member
} dsme_dbus_signal_binding_t;

bool dsme_dbus_is_available(void);
DBusConnection *dsme_dbus_get_connection(DBusError *err);

void dsme_dbus_bind_methods(bool*                      bound_already,
                            const dsme_dbus_binding_t* bindings,
                            const char*                service,
                            const char*                interface);

void dsme_dbus_unbind_methods(bool*                      really_bound,
                              const dsme_dbus_binding_t* bindings,
                              const char*                service,
                              const char*                interface);

void dsme_dbus_bind_signals(bool*                             bound_already,
                            const dsme_dbus_signal_binding_t* bindings);

void dsme_dbus_unbind_signals(bool*                             really_bound,
                              const dsme_dbus_signal_binding_t* bindings);

DsmeDbusMessage* dsme_dbus_reply_new(const DsmeDbusMessage* request);

DsmeDbusMessage* dsme_dbus_signal_new(const char* path,
                                      const char* interface,
                                      const char* name);

void dsme_dbus_message_append_string(DsmeDbusMessage* msg, const char* s);
void dsme_dbus_message_append_int(DsmeDbusMessage* msg, int i);

int         dsme_dbus_message_get_int(const DsmeDbusMessage* msg);
const char* dsme_dbus_message_get_string(const DsmeDbusMessage* msg);
bool        dsme_dbus_message_get_bool(const DsmeDbusMessage* msg);
bool        dsme_dbus_message_get_variant_bool(const DsmeDbusMessage* msg);
const char* dsme_dbus_message_path(const DsmeDbusMessage* msg);

// NOTE: frees the signal; hence not const
void dsme_dbus_signal_emit(DsmeDbusMessage* sig);

char* dsme_dbus_endpoint_name(const DsmeDbusMessage* request);

DsmeDbusMessage* dsme_dbus_reply_error(const DsmeDbusMessage*  request,
                                       const char*             error_name,
                                       const char*             error_message);

void dsme_dbus_startup(void);
void dsme_dbus_cleanup(void);
#endif
