/**
 * @file red_private.h
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

#ifndef __RED_PRIVATE_H__
#define __RED_PRIVATE_H__

#include <sys/queue.h>

#include "clock.h"
#include "fsm.h"
#include "msg.h"
#include "util.h"
#include "port_private.h"

struct red_port {
	/* === Copied from `struct port`. Needs to be handled on a per-port basis === */
	const char *name;
	char *log_name;
	struct interface *iface;
	struct clock *clock;
	struct transport *trp;
	int fault_fd;
	int phc_index;
	/* TAILQ_HEAD(red_delay_req, ptp_message) delay_req; */
	struct ptp_message *peer_delay_req;
	struct ptp_message *peer_delay_resp;
	struct ptp_message *peer_delay_fup;
	int peer_portid_valid;
	struct PortIdentity peer_portid;
	struct {
		/* UInteger16 announce; */
		UInteger16 delayreq;
		/* UInteger16 signaling; */
		/* UInteger16 sync; */
	} seqnum;
	tmv_t                 peer_delay;
	TimeInterval          peerMeanPathDelay;
	struct tsproc         *tsproc;
	struct nrate_estimator nrate;
	unsigned int          pdr_missing;
	enum port_state       state; /*portState*/
	Integer64             asymmetry;
	Integer64             rx_timestamp_offset;
	Integer64             tx_timestamp_offset;
	enum link_state       link_status;
	struct fault_interval flt_interval_pertype[FT_CNT];
	enum fault_type       last_fault_type;
	struct PortStats      stats;
	int                   dummy_pdelay_resp_fup;
	int                   egress_vlan_tagged;
	int                   egress_vlan_id;
	int                   egress_vlan_prio;
	int                   errorCounter;
	LIST_HEAD(red_fm, foreign_clock) foreign_masters;
	struct foreign_clock *best;

	/* === Special RED variables === */

	struct port *upper;
	/* Set when the port timed out. Unset when an ANNO is recieved */
	bool                  anno_timed_out;
        /* bool                  is_port_a; */
        /* bool                  is_port_b; */
};




#endif /* __RED_PRIVATE_H__ */
