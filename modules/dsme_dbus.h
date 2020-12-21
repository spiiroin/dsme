/**
   @file dsme_dbus.h

   D-Bus C binding for DSME
   <p>
   Copyright (c) 2009 - 2010 Nokia Corporation.
   Copyright (c) 2013 - 2020 Jolla Ltd.
   Copyright (c) 2020 Open Mobile Platform LLC.

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

typedef struct DsmeDbusMessage DsmeDbusMessage;

#define DSME_DBUS_MESSAGE_DUMMY ((DsmeDbusMessage *)(0xaffe0000))

typedef void (*DsmeDbusMethod)(const DsmeDbusMessage* request,
                               DsmeDbusMessage**      reply);

typedef void (*DsmeDbusHandler)(const DsmeDbusMessage* ind);

typedef struct dsme_dbus_binding_t
{
    DsmeDbusMethod  method;
    const char     *name; // = member
    bool            priv;
    const char     *args; // = xml desc
} dsme_dbus_binding_t;

typedef struct dsme_dbus_signal_binding_t
{
    DsmeDbusHandler  handler;
    const char      *interface;
    const char      *name; // = member
} dsme_dbus_signal_binding_t;

DBusConnection *dsme_dbus_get_connection(DBusError *err);

void dsme_dbus_bind_methods  (bool *bound, const char *service_name, const char *object_path, const char *interface_name, const dsme_dbus_binding_t *bindings);
void dsme_dbus_unbind_methods(bool *bound, const char *service_name, const char *object_path, const char *interface_name, const dsme_dbus_binding_t *bindings);
void dsme_dbus_bind_signals  (bool *bound, const dsme_dbus_signal_binding_t *bindings);
void dsme_dbus_unbind_signals(bool *bound, const dsme_dbus_signal_binding_t *bindings);

DsmeDbusMessage* dsme_dbus_reply_new(const DsmeDbusMessage* request);

DsmeDbusMessage* dsme_dbus_signal_new(const char *sender, const char *path, const char *interface, const char *name);

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

bool dsme_dbus_connect(void);
void dsme_dbus_disconnect(void);

void dsme_dbus_startup(void);
void dsme_dbus_shutdown(void);
#endif
