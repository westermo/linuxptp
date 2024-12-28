/**
 * @file tcp_uds.h
 * @note Copyright (C) 2024 Casper Andersson <casper.casan@gmail.com>
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

#ifndef __TCPUDS_H__
#define __TCPUDS_H__

#define TCPUDS_TX_BUF_SIZE 5000
#define TCPUDS_RX_BUFFER_SIZE 5000

#define TCPUDS_MATCH(fd, buffer, field, access, formatter, request) { \
	if (!request || strncmp(field, request, sizeof(field)) == 0) { \
		count = snprintf(buffer, TCPUDS_TX_BUF_SIZE, "%s %" formatter "\n", field, access); \
		send(fd, buffer, count, 0);				\
	}								\
} 

int tcpuds_open(struct interface *interface);
void tcpuds_close(int fd);
int tcpuds_rcv(struct clock *c, int fd);

#endif /* __TCPUDS_H__ */
