/**
 * @file tcp_uds.c
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

#include <inttypes.h>
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <unistd.h>
#include <sys/queue.h>

#include "clock.h"
#include "port_private.h"
#include "print.h"
#include "tcp_uds.h"
#include "msg.h"

/* A fast UDS socket using TCP and only for local communication.
 * Fast in comparison to the normal PMC commands that just send a
 * request into a black hole without knowing what will come back (and
 * therefore has to wait some time). This is for local commands only
 * and should not be forwarded.
 */

#define TCP_BACKLOG 3

static char path[108];

const char *dm_str[] = {
	"Auto",
	"E2E",
	"P2P",
	"NONE",
};

static char *clock_type_to_str(enum clock_type type)
{
	switch (type) {
	case CLOCK_TYPE_ORDINARY:
		return "OC";
	case CLOCK_TYPE_BOUNDARY:
		return "BC";
	case CLOCK_TYPE_P2P:
		return "TC_P2P";
	case CLOCK_TYPE_E2E:
		return "TC_E2E";
	case CLOCK_TYPE_MANAGEMENT:
		return "MANAGEMENT";
	}
	return "UNKNOWN";
}

int tcpuds_open(struct interface *interface)
{
	// Open and bind to socket
	struct sockaddr_un sockaddr_un = { 0 };
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		pr_err("%s: socket(): %s", __func__, strerror(errno));
		return 0;
	}

	/* Construct the bind address structure. */
	sockaddr_un.sun_family = AF_UNIX;
	/* 108 comes from definition of sun_path */
	snprintf(sockaddr_un.sun_path, 108, "%s-tcp", interface_name(interface));
	/* Remove (maybe) a prior run. */
	remove(sockaddr_un.sun_path);

	/* If socket_address exists on the filesystem, then bind will fail. */
	if (bind(fd, (struct sockaddr *)&sockaddr_un, sizeof(struct sockaddr_un))) {
		pr_err("%s: bind(): %s", __func__, strerror(errno));
		return 0;
	}
	
	if (listen(fd, TCP_BACKLOG) < 0) {
		pr_err("%s: listen(): %s", __func__, strerror(errno));
		return 0;
	}
	
	return fd;
}

void tcpuds_close(int fd)
{
	close(fd);
	remove(path);
}

static void tcpuds_end_message(int fd)
{
	const char end[] = "\n";
	send(fd, end, sizeof(end), 0);
}

static int tcpuds_port_ds(struct port *p, int fd, const char *request)
{
	char buffer[TCPUDS_TX_BUF_SIZE];
	int count;
	
	/* Send port header */
	snprintf(buffer, TCPUDS_TX_BUF_SIZE, "PORT:%s\n", p->name);
	send(fd, buffer, strlen(buffer), 0);

	TCPUDS_MATCH(fd, buffer, "portIdentity", pid2str(&p->portIdentity), "s", request);
	TCPUDS_MATCH(fd, buffer, "portState", ps_str[p->state], "s", request);
	TCPUDS_MATCH(fd, buffer, "logMinDelayReqInterval", p->logMinDelayReqInterval, PRId8, request);
	TCPUDS_MATCH(fd, buffer, "peerMeanPathDelay", p->peer_delay.ns, PRId64, request);
	TCPUDS_MATCH(fd, buffer, "logAnnounceInterval", p->logAnnounceInterval, PRId8, request);
	TCPUDS_MATCH(fd, buffer, "announceReceiptTimeout", p->announceReceiptTimeout, PRIu8, request);
	TCPUDS_MATCH(fd, buffer, "logSyncInterval", p->logSyncInterval, PRId8, request);
	TCPUDS_MATCH(fd, buffer, "delayMechanism", dm_str[p->delayMechanism], "s", request);
	TCPUDS_MATCH(fd, buffer, "logMinPdelayReqInterval", p->logMinPdelayReqInterval, PRId8, request);
	TCPUDS_MATCH(fd, buffer, "versionNumber", ptp_hdr_ver & 0x7, PRIu8, request);
	TCPUDS_MATCH(fd, buffer, "minorVersionNumber", ptp_hdr_ver >> 4, PRIu8, request);
	
	return 0;
}

/* static const char *getPhysicalAddress(struct port *p) */
/* { */
/* 	static char str[100]; */
/* 	uint8_t addr[16]; */
/* 	uint16_t val; */

/* 	val = transport_physical_addr(p->trp, addr); */
/* 	snprintf(str, 100, "%02x:%02x:%02x:%02x:%02x:%02x", */
/* 		addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]); */
/* 		/\* memcpy(&cd->physicalAddress->length, &val, 2); *\/ */
/* 		/\* buf += sizeof(struct PhysicalAddress) + val; *\/ */
/* 	return str; */

/* } */
/* static const char *getTransportProtocol(struct port *p) */
/* { */

/* 	uint16_t val; */

/* 		cd->protocolAddress = (struct PortAddress *) buf; */
/* 		u16 = transport_type(target->trp); */
/* 		memcpy(&cd->protocolAddress->networkProtocol, &u16, 2); */
/* 		val = transport_protocol_addr(p->trp, */
/*                                               cd->protocolAddress->address); */
/* 		memcpy(&cd->protocolAddress->addressLength, &val, 2); */
/* 		/\* buf += sizeof(struct PortAddress) + val; *\/ */
/* } */


/* clock_description is a per-port dataset */
int tcpuds_clock_description(struct port *p, int fd, const char *request)
{
	char buffer[TCPUDS_TX_BUF_SIZE];
	struct clock *c = p->clock;
	struct clock_description *desc;
	int count;

	desc = clock_description(c);
	snprintf(buffer, TCPUDS_TX_BUF_SIZE, "CLOCK_DESCRIPTION\n");
	send(fd, buffer, strlen(buffer), 0);

	TCPUDS_MATCH(fd, buffer, "clockType", clock_type_to_str(clock_type(c)), "s", request);
	/* TCPUDS_MATCH(fd, buffer, "physicalLayerProtocol", "IEEE 802.3", "s", request); */
	/* TCPUDS_MATCH(fd, buffer, "physicalAddress", getPhysicalAddress(p), "s", request); */
	/* TCPUDS_MATCH(fd, buffer, "protocolAddress", c->cur.meanPathDelay, PRId64, request); */
	/* TCPUDS_MATCH(fd, buffer, "manufacturerId", c->cur.meanPathDelay, PRId64, request); */
	TCPUDS_MATCH(fd, buffer, "productDescription", desc->productDescription.text, "s", request);
	/* TCPUDS_MATCH(fd, buffer, "revisionData", c->cur.meanPathDelay, PRId64, request); */
	TCPUDS_MATCH(fd, buffer, "userDescription", desc->userDescription.text, "s", request);
	/* TCPUDS_MATCH(fd, buffer, "profileId", c->cur.meanPathDelay, PRId64, request); */
	
	return 0;
}

LIST_HEAD(ports_head, port);

static struct port *get_port_by_name(struct clock *c, char *name)
{
	struct port *piter;

	LIST_FOREACH(piter, clock_get_ports(c), list) {
		if (strncmp(piter->name, name, IFNAMSIZ) == 0)
			return piter;
	}
	return NULL;
}

static struct port *get_first_port(struct clock *c)
{
	struct port *piter;

	LIST_FOREACH(piter, clock_get_ports(c), list) {
		return piter;
	}
	return NULL;
}


static int tcpuds_handle_request(struct clock *c, int fd, char *cmd)
{
	struct port *p = NULL;
	// port.peer_delay
	// port:eth5.peer_delay
	if (strncmp("port", cmd, 4) == 0) {
		cmd += 4;
		char *portname = strchr(cmd, ':');
		char *subfield = strchr(cmd, '.');
		if (subfield) {
			subfield[0] = '\0';
			subfield++;
		}
		if (portname) {
			portname++;
			p = get_port_by_name(c, portname);
			if (!p) {
				// Invalid port name
				return -1;
			}
			portname++;
			tcpuds_port_ds(p, fd, subfield);
		} else {
			struct port *piter;
			LIST_FOREACH(piter, clock_get_ports(c), list) {
				tcpuds_port_ds(piter, fd, subfield);
			}
		}
	} else if (strncmp("current", cmd, 7) == 0) {
		cmd += 7;
		char *subfield = strchr(cmd, '.');
		if (subfield) {
			subfield[0] = '\0';
			subfield++;
		}
		clock_tcpuds_current_ds(c, fd, subfield);
	} else if (strncmp("clock_description", cmd, 7) == 0) {
                /* clock_description is a per-port dataset, and should
                 * ideally use the same approach as port_data_set.
		 */
		cmd += 7;
		char *subfield = strchr(cmd, '.');
		p = get_first_port(c);
		if (subfield) {
			subfield[0] = '\0';
			subfield++;
		}
		tcpuds_clock_description(p, fd, subfield);
	} else if (strncmp("default", cmd, 7) == 0) {
		cmd += 7;
		char *subfield = strchr(cmd, '.');
		if (subfield) {
			subfield[0] = '\0';
			subfield++;
		}
		clock_tcpuds_default_ds(c, fd, subfield);
	}

	return 0;
}

int tcpuds_rcv(struct clock *c, int server_fd)
{
	char buffer[TCPUDS_RX_BUFFER_SIZE];
	int bytes_parsed = 0;
	int count;
	char *req;
	int pos;
	int len;
	int fd;
	
	fd = accept(server_fd, NULL, NULL );
	if (fd == -1) {
		pr_err("Failed to accept tcpuds connection");
		return -1;
	}
	
	count = recv(fd, buffer, TCPUDS_RX_BUFFER_SIZE, 0);
	for (pos = 0; pos < count; pos++)
		if (buffer[pos] == '\n')
			buffer[pos] = '\0';
	

	req = buffer;
	while (bytes_parsed < count) {
		len = strlen(req);
		bytes_parsed += len + 1;
		tcpuds_handle_request(c, fd, req);
		req = buffer + bytes_parsed;
	}

	tcpuds_end_message(fd);
	close(fd);
	return 0;
}

