/**
   @file diskmonitor.h

   <p>
   Copyright (C) 2011 Nokia Corporation.
   Copyright (C) 2017 Jolla Ltd

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

#ifndef DSME_DISKMONITOR_H
#define DSME_DISKMONITOR_H

#include <stdbool.h>

#include <dsme/messages.h>

typedef enum
{
    DISKSPACE_STATE_UNSET   = -2,
    DISKSPACE_STATE_UNDEF   = -1,
    DISKSPACE_STATE_NORMAL  =  0,
    DISKSPACE_STATE_WARNING =  1,
} diskspace_state_t;

const char *diskspace_state_repr(diskspace_state_t state);

typedef struct {
  DSMEMSG_PRIVATE_FIELDS

  /* Logical disk use state */
  diskspace_state_t diskspace_state;

  // mount_path is passed in extra.
} DSM_MSGTYPE_DISK_SPACE;

enum {
  /* NOTE: dsme message types are defined in:
   * - libdsme
   * - libiphb
   * - dsme
   *
   * When adding new message types
   * 1) uniqueness of the identifiers must be
   *    ensured accross all these source trees
   * 2) the dsmemsg_id_name() function in libdsme
   *    must be made aware of the new message type
   */

  DSME_MSG_ENUM(DSM_MSGTYPE_DISK_SPACE, 0x00002000),
};

#endif
