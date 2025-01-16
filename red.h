/**
 * @file red.h
 * @note Copyright (C) 2025 Casper Andersson <casper.casan@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __RED_H__
#define __RED_H__

#include "clock.h"
/* #include "fsm.h" */
/* #include "msg.h" */
/* #include "util.h" */
/* #include "port_private.h" */

/* struct red { */
/* 	struct red_port pa; */
/* 	struct red_port pb; */
/* }; */


struct port *red_open(const char *phc_device,
		      int phc_index,
		      enum timestamp_type timestamping,
		      int number,
		      struct interface *iface_a,
		      struct interface *iface_b,
		      struct clock *clock);
void red_close(struct port *p);
struct foreign_clock *red_compute_best(struct port *p);

#endif /* __RED_H__ */
