/**
   @file dsme-server.h

   This file implements the main function and main loop of DSME component.
   <p>
   Copyright (C) 2017 Jolla Ltd

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

#ifndef  DSME_SERVER_H_
# define DSME_SERVER_H_

# include <stdbool.h>

bool dsme_in_valgrind_mode(void);

#endif /* DSME_SERVER_H_ */
