/**
 * @file tc.c
 * @note Copyright (C) 2018 Richard Cochran <richardcochran@gmail.com>
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
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA.
 */
#include <stdlib.h>

#include "clock.h"
#include "ddt.h"
#include "ds.h"
#include "fsm.h"
#include "msg.h"
#include "port.h"
#include "print.h"
#include "tc.h"
#include "tmv.h"
#include "util.h"

enum tc_match {
	TC_MISMATCH,
	TC_SYNC_FUP,
	TC_FUP_SYNC,
	TC_DELAY_REQRESP,
};

static void tc_hsr_set_port_identity(struct port *q, struct port *p,
				     struct ptp_message *msg, struct PortIdentity *tmp,
				     bool set)
{
	struct port *master = p;

	/* Use identity of port A */
	if (port_hsr_prp_b(p))
		master = port_get_paired(p);

	/* Ring injection requires setting the sourcePortIdentity to the TC clock and port */
	if (set && !port_get_paired(q) && port_get_paired(master)) {
		*tmp = msg->header.sourcePortIdentity;
		msg->header.sourcePortIdentity.clockIdentity = master->portIdentity.clockIdentity;
		msg->header.sourcePortIdentity.portNumber = htons(master->portIdentity.portNumber);
	} else if (!set && !port_get_paired(q) && port_get_paired(master)) {
		msg->header.sourcePortIdentity = *tmp;
	}
}

static void tc_prp_set_port_number_bits(struct port *from, struct port *to, struct ptp_message *msg, bool set)
{
	if (port_delay_mechanism(from) != DM_E2E)
		return;

	// From interlink to A/B, clear portNumber bits
	if (port_hsr_prp_a(to) || port_hsr_prp_b(to) || !set) {
		msg->header.sourcePortIdentity.portNumber &= ~htons(0b11 << 12);
		return;
	}

	// From A/B to interlink
	if (port_hsr_prp_a(from)) {
		// Set portNumber 10
		msg->header.sourcePortIdentity.portNumber |= htons(0b10 << 12);
	} else if (port_hsr_prp_b(from)) {
		// Set portNumber 11
		msg->header.sourcePortIdentity.portNumber |= htons(0b11 << 12);
	}
}

static void tc_prp_clear_resp_port_number_bits(struct port *from, struct ptp_message *msg)
{
	if (port_delay_mechanism(from) != DM_E2E)
		return;

	// From interlink to A/B, clear portNumber bits
	if (msg_type(msg) == DELAY_RESP)
		msg->delay_resp.requestingPortIdentity.portNumber &= ~htons(0b11 << 12);
}

bool tc_prp_resp_is_lan(struct ptp_message *msg, UInteger16 lan_bits)
{
	UInteger16 portno; 

	if (msg_type(msg) != DELAY_RESP)
		return false;

	portno = msg->delay_resp.requestingPortIdentity.portNumber;
	return (ntohs(portno) & PRP_LAN_BITMASK) == lan_bits;
}

static bool tc_hsr_prp_should_use_port(struct port *p)
{
	struct port *pair;

	/* Send on port A and have it duplicated in HW */
	if (port_hsr_prp_a(p)) {
		return true;
	} else if (port_hsr_prp_b(p)) {
		/* If port A is down we send on port B. Also do if A
		 * is PASSIVE_SLAVE since that allows us to preserve
		 * tc_ignore rules for upstream DelayReq in PRP (only
		 * DelayReq will egress on a PASSIVE_SLAVE port). It
		 * will be duplicated in HW, we just send on the B
		 * port.
		 *
		 * If A is LISTENING we still want to send on B to
		 * reduce switchover delay.
		 */
		pair = port_get_paired(p);
		switch(pair->state) {
		case PS_DISABLED:
		case PS_FAULTY:
		case PS_PASSIVE_SLAVE:
		case PS_LISTENING:
			return true;
		default:
			break;
		}
		return false;
	}
	return true;
}

static bool tc_prp_should_fwd(struct port *q, struct port *p, struct ptp_message *msg)
{
	/* Into PRP nets */
	if (!port_get_paired(q) && port_get_paired(p)) {
		if (msg_type(msg) == DELAY_RESP) {
			/* E2E mode: DelayResp must forward
			 * independently, like Pdelay. Requires
			 * duplication algorithm in kernel/HW to be aware
			 * of this */
			if (port_hsr_prp_a(p) && tc_prp_resp_is_lan(msg, PRP_LAN_A_BITS)) {
				return true;
			} else if (port_hsr_prp_b(p) && tc_prp_resp_is_lan(msg, PRP_LAN_B_BITS)) {
				return true;
			} else {
				return false;
			}
		}
		return tc_hsr_prp_should_use_port(p);
	}

	return true;
}
static bool tc_hsr_should_fwd(struct port *q, struct port *p,
					struct ptp_message *msg)
{
	struct PortIdentity parent = clock_parent_identity(q->clock);
	struct PortIdentity tmp;

	/* Forwarding within the ring happens in HW and is prevented
	 * in SW through the normal port states. */

	/* Into ring */
	if (!port_get_paired(q) && port_get_paired(p)) {
		return tc_hsr_prp_should_use_port(p);
	}

	/* Out from ring */
	if (port_get_paired(q) && !port_get_paired(p)) {
		tmp = parent;
		tmp.portNumber = htons(tmp.portNumber);
		if(!pid_eq(&tmp, &msg->header.sourcePortIdentity)) {
			return false;
		}
		// TODO: Handle Delay_Req? Is HSR used with E2E?
		//if (q->state == PS_MASTER) {
		//	/* GM-attached. Don't forward to other GM.
		//	 * According to standard, this case should be
		//	 * handled by HSR not forwarding due to
		//	 * pathId. But since that probably isn't possible
		//	 * we need to prevent it here to the best of our
		//	 * ability. We will still forward there until
		//	 * we have reached the Master state.
		//	 */
		//	return false;
		//} else {
		//	// Slave-attached
		//	if (q->state == PS_PASSIVE_SLAVE) {
		//		return false;
		//	}
		//	// XXX: check_source_identity ??
		//	tmp = parent;
		//	tmp.portNumber = htons(tmp.portNumber);
		//	if(!pid_eq(&tmp, &msg->header.sourcePortIdentity)) {
		//		return false;
		//	}
		//}
	}

	return true;
}

static TAILQ_HEAD(tc_pool, tc_txd) tc_pool = TAILQ_HEAD_INITIALIZER(tc_pool);

static int tc_match_delay(int ingress_port, struct ptp_message *resp,
			  struct tc_txd *txd);
static int tc_match_syfup(int ingress_port, struct ptp_message *msg,
			  struct tc_txd *txd);
static void tc_recycle(struct tc_txd *txd);

static struct tc_txd *tc_allocate(void)
{
	struct tc_txd *txd = TAILQ_FIRST(&tc_pool);

	if (txd) {
		TAILQ_REMOVE(&tc_pool, txd, list);
		memset(txd, 0, sizeof(*txd));
		return txd;
	}
	txd = calloc(1, sizeof(*txd));
	return txd;
}

int tc_hsr_prp_blocked(struct port *p, enum port_state s)
{
	/* Forwarding to PASSIVE and PASSIVE_SLAVE is acceptable for HSR/PRP */
	// XXX: Is this correct?
	switch (s) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
		return 1;
	default:
		return 0;
	}
}

int tc_blocked(struct port *q, struct port *p, struct ptp_message *m)
{
	enum port_state s;

	if (q == p) {
		return 1;
	}
	if (portnum(p) == 0) {
		return 1;
	}
	if (!q->tc_spanning_tree && !clock_is_hsr(p->clock)) {
		return 0;
	}
	/* Forward frames in the wrong domain unconditionally. */
	if (m->header.domainNumber != clock_domain_number(p->clock)) {
		return 0;
	}

	if (!portnum(q) && port_state(q) != PS_FAULTY)
		goto egress;

	/* Ingress state */
	s = port_state(q);
	switch (s) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_PASSIVE:
	case PS_PASSIVE_SLAVE:
		return 1;
	case PS_MASTER:
	case PS_GRAND_MASTER:
		/* Delay_Req and Management swims against the stream. */
		switch(msg_type(m)) {
		case DELAY_REQ:
		case MANAGEMENT:
			break;
		default:
			return 1;
		}
		break;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}

egress:
	/* Egress state */
	s = port_state(p);
	switch (s) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_PASSIVE:
	case PS_PASSIVE_SLAVE:
		return 1;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		/* Delay_Req does and Management may
		 * swims against the stream. */
		switch(msg_type(m)) {
		case DELAY_REQ:
		case MANAGEMENT:
			break;
		default:
			return 1;
		}
		break;
	case PS_MASTER:
	case PS_GRAND_MASTER:
		/* No use forwarding Delay_Req out the wrong port. */
		if (msg_type(m) == DELAY_REQ) {
			return 1;
		}
		break;
	}

	return 0;
}

static void tc_complete_request(struct port *q, struct port *p,
				struct ptp_message *req, tmv_t residence)
{
	struct tc_txd *txd = tc_allocate();
	if (!txd) {
		port_dispatch(p, EV_FAULT_DETECTED, 0);
		return;
	}
#ifdef DEBUG
	pr_err("stash delay request from %s to %s seqid %hu residence %lu",
	       q->log_name, p->log_name, ntohs(req->header.sequenceId),
	       (unsigned long) tmv_to_nanoseconds(residence));
#endif
	msg_get(req);
	txd->msg = req;
	txd->residence = residence;
	txd->ingress_port = portnum(q);
	TAILQ_INSERT_TAIL(&p->tc_transmitted, txd, list);
}

static void tc_complete_response(struct port *q, struct port *p,
				 struct ptp_message *resp, tmv_t residence)
{
	enum tc_match type = TC_MISMATCH;
	struct tc_txd *txd;
	Integer64 c1, c2;
	int cnt;

#ifdef DEBUG
	pr_err("complete delay response from %s to %s seqid %hu",
	       q->log_name, p->log_name, ntohs(resp->header.sequenceId));
#endif
	TAILQ_FOREACH(txd, &q->tc_transmitted, list) {
		type = tc_match_delay(portnum(p), resp, txd);
		if (type == TC_DELAY_REQRESP) {
			residence = txd->residence;
			break;
		}
	}
	if (type != TC_DELAY_REQRESP) {
		return;
	}
	c1 = net2host64(resp->header.correction);
	c2 = c1 + tmv_to_TimeInterval(residence);
	resp->header.correction = host2net64(c2);
	cnt = transport_send(p->trp, &p->fda, TRANS_GENERAL, resp);
	if (cnt <= 0) {
		pr_err("tc failed to forward response on %s", p->log_name);
		p->errorCounter++;
		port_dispatch(p, EV_FAULT_DETECTED, 0);
	}
	/* Restore original correction value for next egress port. */
	resp->header.correction = host2net64(c1);
	TAILQ_REMOVE(&q->tc_transmitted, txd, list);
	msg_put(txd->msg);
	tc_recycle(txd);
}

static void tc_complete_syfup(struct port *q, struct port *p,
			      struct ptp_message *msg, tmv_t residence)
{
	enum tc_match type = TC_MISMATCH;
	struct ptp_message *fup;
	struct tc_txd *txd;
	Integer64 c1, c2;
	int cnt;

	TAILQ_FOREACH(txd, &p->tc_transmitted, list) {
		type = tc_match_syfup(portnum(q), msg, txd);
		switch (type) {
		case TC_MISMATCH:
			break;
		case TC_SYNC_FUP:
			fup = msg;
			residence = txd->residence;
			break;
		case TC_FUP_SYNC:
			fup = txd->msg;
			break;
		case TC_DELAY_REQRESP:
			pr_err("tc: unexpected match of delay request - sync!");
			return;
		}
		if (type != TC_MISMATCH) {
			break;
		}
	}

	if (type == TC_MISMATCH) {
		txd = tc_allocate();
		if (!txd) {
			port_dispatch(p, EV_FAULT_DETECTED, 0);
			return;
		}
		msg_get(msg);
		txd->msg = msg;
		txd->residence = residence;
		txd->ingress_port = portnum(q);
		TAILQ_INSERT_TAIL(&p->tc_transmitted, txd, list);
		return;
	}

	c1 = net2host64(fup->header.correction);
	c2 = c1 + tmv_to_TimeInterval(residence);
	c2 += tmv_to_TimeInterval(q->peer_delay);
	c2 += q->asymmetry;
	fup->header.correction = host2net64(c2);

	cnt = transport_send(p->trp, &p->fda, TRANS_GENERAL, fup);
	if (cnt <= 0) {
		pr_err("tc failed to forward follow up on %s", p->log_name);
		p->errorCounter++;
		port_dispatch(p, EV_FAULT_DETECTED, 0);
	}
	/* Restore original correction value for next egress port. */
	fup->header.correction = host2net64(c1);

	TAILQ_REMOVE(&p->tc_transmitted, txd, list);
	msg_put(txd->msg);
	tc_recycle(txd);
}

static void tc_complete(struct port *q, struct port *p,
			struct ptp_message *msg, tmv_t residence)
{
	switch (msg_type(msg)) {
	case SYNC:
	case FOLLOW_UP:
		tc_complete_syfup(q, p, msg, residence);
		break;
	case DELAY_REQ:
		tc_complete_request(q, p, msg, residence);
		break;
	case DELAY_RESP:
		tc_complete_response(q, p, msg, residence);
		break;
	}
}

static int tc_current(struct ptp_message *m, struct timespec now)
{
	int64_t t1, t2;

	t1 = m->ts.host.tv_sec * NSEC_PER_SEC + m->ts.host.tv_nsec;
	t2 = now.tv_sec * NSEC_PER_SEC + now.tv_nsec;

	return t2 - t1 < NSEC_PER_SEC;
}

static int tc_fwd_event(struct port *q, struct ptp_message *msg)
{
	tmv_t egress, ingress = msg->hwts.ts, residence;
	struct PortIdentity tmp;
	struct port *p;
	int cnt, err;
	double rr;
	Integer64 corr, orig_corr;

	clock_gettime(CLOCK_MONOTONIC, &msg->ts.host);

	// Should this happen for DELAY_REQ/RESP too? At least the asymmetry and offset
	if ((q->timestamping >= TS_ONESTEP) && (msg_type(msg) == SYNC)) {
		corr = net2host64(msg->header.correction);
		corr += tmv_to_TimeInterval(q->peer_delay);
		corr += q->asymmetry;
		corr += q->rx_timestamp_offset;
		msg->header.correction = host2net64(corr);
	}
	orig_corr = msg->header.correction;

	/* First send the event message out. */
	for (p = clock_first_port(q->clock); p; p = LIST_NEXT(p, list)) {
		if (tc_blocked(q, p, msg)) {
			continue;
		}
		if ((q->timestamping >= TS_ONESTEP) && (msg_type(msg) == SYNC)) {
			corr = net2host64(msg->header.correction);
			corr += p->tx_timestamp_offset;
			msg->header.correction = host2net64(corr);
		}
		if (clock_is_hsr(q->clock)) {
			if (!tc_hsr_should_fwd(q, p, msg))
				continue;
			tc_hsr_set_port_identity(q, p, msg, &tmp, true);
		}
		if (clock_is_prp(q->clock)) {
			if (!tc_prp_should_fwd(q, p, msg))
				continue;
			tc_prp_set_port_number_bits(q, p, msg, true);
		}

		cnt = transport_send(p->trp, &p->fda, TRANS_DEFER_EVENT, msg);
		if (cnt <= 0) {
			pr_err("failed to forward event from %s to %s",
				q->log_name, p->log_name);
			p->errorCounter++;
			port_dispatch(p, EV_FAULT_DETECTED, 0);
		}

		msg->header.correction = orig_corr;

		if (clock_is_hsr(q->clock)) {
			tc_hsr_set_port_identity(q, p, msg, &tmp, false);
		}
		if (clock_is_prp(q->clock)) {
			tc_prp_set_port_number_bits(q, p, msg, false);
		}
	}

	if (q->timestamping >= TS_ONESTEP) {
		goto onestep;
	}

	/* Go back and gather the transmit time stamps. */
	for (p = clock_first_port(q->clock); p; p = LIST_NEXT(p, list)) {
		if (tc_blocked(q, p, msg)) {
			continue;
		}
		err = transport_txts(&p->fda, msg);
		if (err || !msg_sots_valid(msg)) {
			pr_err("failed to fetch txts on %s to %s event",
				q->log_name, p->log_name);
			port_dispatch(p, EV_FAULT_DETECTED, 0);
			continue;
		}
		ts_add(&msg->hwts.ts, p->tx_timestamp_offset);
		egress = msg->hwts.ts;
		residence = tmv_sub(egress, ingress);
		rr = clock_rate_ratio(q->clock);
		if (rr != 1.0) {
			residence = dbl_tmv(tmv_dbl(residence) * rr);
		}
		tc_complete(q, p, msg, residence);
	}

 onestep:
	return 0;
}

static int tc_match_delay(int ingress_port, struct ptp_message *resp,
			  struct tc_txd *txd)
{
	struct ptp_message *req = txd->msg;

	if (ingress_port != txd->ingress_port) {
		return TC_MISMATCH;
	}
	if (req->header.sequenceId != resp->header.sequenceId) {
		return TC_MISMATCH;
	}
	if (!pid_eq(&req->header.sourcePortIdentity,
		    &resp->delay_resp.requestingPortIdentity)) {
		return TC_MISMATCH;
	}
	if (msg_type(req) == DELAY_REQ && msg_type(resp) == DELAY_RESP) {
		return TC_DELAY_REQRESP;
	}
	return TC_MISMATCH;
}

static int tc_match_syfup(int ingress_port, struct ptp_message *msg,
			  struct tc_txd *txd)
{
	if (ingress_port != txd->ingress_port) {
		return TC_MISMATCH;
	}
	if (msg->header.sequenceId != txd->msg->header.sequenceId) {
		return TC_MISMATCH;
	}
	if (!source_pid_eq(msg, txd->msg)) {
		return TC_MISMATCH;
	}
	if (msg_type(txd->msg) == SYNC && msg_type(msg) == FOLLOW_UP) {
		return TC_SYNC_FUP;
	}
	if (msg_type(txd->msg) == FOLLOW_UP && msg_type(msg) == SYNC) {
		return TC_FUP_SYNC;
	}
	return TC_MISMATCH;
}

static void tc_recycle(struct tc_txd *txd)
{
	TAILQ_INSERT_HEAD(&tc_pool, txd, list);
}

/* public methods */

void tc_cleanup(void)
{
	struct tc_txd *txd;

	while ((txd = TAILQ_FIRST(&tc_pool)) != NULL) {
		TAILQ_REMOVE(&tc_pool, txd, list);
		free(txd);
	}
}

void tc_flush(struct port *q)
{
	struct tc_txd *txd;

	while ((txd = TAILQ_FIRST(&q->tc_transmitted)) != NULL) {
		TAILQ_REMOVE(&q->tc_transmitted, txd, list);
		msg_put(txd->msg);
		tc_recycle(txd);
	}
}


static int forwarding(struct clock *c, struct port *p)
{
	enum port_state ps = port_state(p);
	switch (ps) {
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_SLAVE:
	case PS_UNCALIBRATED:
	case PS_PRE_MASTER:
		return 1;
	default:
		break;
	}
	if (p == clock_uds_rw_port(c) && ps != PS_FAULTY) {
		return 1;
	}
	return 0;
}

int tc_manage(struct port *q, struct ptp_message *msg)
{
	struct port *p;
	int cnt;
	int pdulen;

	if (forwarding(q->clock, q) && msg->management.boundaryHops) {
		msg->management.boundaryHops = 0;
		pdulen = msg->header.messageLength;
		p = clock_uds_rw_port(q->clock);

		/*
		 * Accept management respones to uds port
		 */
		switch (management_action(msg)) {
		case GET:
		case SET:
		case COMMAND:
			break;
		case RESPONSE:
		case ACKNOWLEDGE:
			msg_pre_send(msg);
			cnt = transport_send(p->trp, &p->fda, TRANS_GENERAL, msg);
			if (cnt <= 0)
				pr_err("tc failed to forward message to uds port");
			else
				pr_err("response processed");
			msg_post_recv(msg, pdulen);
		}
	}
	return clock_do_manage(q->clock, q, msg);
}

int tc_forward(struct port *q, struct ptp_message *msg)
{
	struct PortIdentity tmp;
	uint16_t steps_removed;
	struct port *p;
	int cnt;

	if (q->tc_spanning_tree && msg_type(msg) == ANNOUNCE) {
		steps_removed = ntohs(msg->announce.stepsRemoved);
		msg->announce.stepsRemoved = htons(1 + steps_removed);
	} else if ((clock_is_hsr(q->clock) || clock_is_prp(q->clock)) && msg_type(msg) == MANAGEMENT) {
		/* HSR forwards in HW inside the ring, causing a huge
		 * amount of packages since all requests and responses
		 * are basically broadcast. Let's not forward them for
		 * now.
		 */
		return 0;
	}

	for (p = clock_first_port(q->clock); p; p = LIST_NEXT(p, list)) {
		if (tc_blocked(q, p, msg)) {
			continue;
		}
		/* Management packets need to retain their identity,
		 * else everything behind it will appear as the same
		 * clock. This is not mentioned in the HSR/PRP standard.
		 */
		if (clock_is_hsr(q->clock)) {
			if (!tc_hsr_should_fwd(q, p, msg)) {
				continue;
			}
			if (msg_type(msg) != MANAGEMENT) {
			    tc_hsr_set_port_identity(q, p, msg, &tmp, true);
			}
		}
		if (clock_is_prp(q->clock)) {
			if (!tc_prp_should_fwd(q, p, msg))
				continue;
			tc_prp_set_port_number_bits(q, p, msg, true);
		}
		cnt = transport_send(p->trp, &p->fda, TRANS_GENERAL, msg);
		if (cnt <= 0) {
			pr_err("tc failed to forward message on %s",
			       p->log_name);
			p->errorCounter++;
			port_dispatch(p, EV_FAULT_DETECTED, 0);
		}
		if (clock_is_hsr(q->clock)) {
			tc_hsr_set_port_identity(q, p, msg, &tmp, false);
		}
		if (clock_is_prp(q->clock)) {
			tc_prp_set_port_number_bits(q, p, msg, false);
		}
	}
	return 0;
}

static int tc_twostep_to_onestep_syfup(struct port *q, struct ptp_message *msg)
{
	UInteger16 seq_id;
	UInteger64 corr;

	seq_id = ntohs(msg->header.sequenceId);

	if (q->onestep_info.seq_id != seq_id || !q->onestep_info.valid) {
		/* Save information until we get the corresponding Sync/Fup */
		q->onestep_info.originTimestamp = msg->sync.originTimestamp;
		q->onestep_info.correction = msg->header.correction;
		q->onestep_info.reserved2 = msg->header.reserved2;
		q->onestep_info.msg_type = msg_type(msg);
		q->onestep_info.seq_id = seq_id;
		q->onestep_info.valid = true;
		return 0;
	}

	/* Include correction from both Sync and Fup */
	corr = net2host64(msg->header.correction);
	corr += net2host64(q->onestep_info.correction);
	msg->header.correction = host2net64(corr);

	if (q->onestep_info.msg_type == SYNC && msg_type(msg) == FOLLOW_UP) {
		// Got Sync first, send Fup as Sync
		msg->header.reserved2 = q->onestep_info.reserved2;
		msg->header.tsmt = SYNC | q->transportSpecific;
		/* controlField is deprecated, but in case GM sends
		* Fup with it set we should set it to 0 for the Sync.
		*/
		msg->header.control = 0;
	} else if (q->onestep_info.msg_type == FOLLOW_UP && msg_type(msg) == SYNC) {
		// Got Fup first, send Sync with Fup info
		msg->sync.originTimestamp = q->onestep_info.originTimestamp;
		msg->header.flagField[0] &= ~TWO_STEP;
	}

	q->onestep_info.valid = false;
	return tc_fwd_event(q, msg);
}

int tc_fwd_folup(struct port *q, struct ptp_message *msg)
{
	struct port *p;

	if (q->timestamping >= TS_ONESTEP) {
		return tc_twostep_to_onestep_syfup(q, msg);
	}

	clock_gettime(CLOCK_MONOTONIC, &msg->ts.host);

	for (p = clock_first_port(q->clock); p; p = LIST_NEXT(p, list)) {
		if (tc_blocked(q, p, msg)) {
			continue;
		}
		tc_complete(q, p, msg, tmv_zero());
	}
	return 0;
}

int tc_fwd_request(struct port *q, struct ptp_message *msg)
{
	return tc_fwd_event(q, msg);
}

int tc_fwd_response(struct port *q, struct ptp_message *msg)
{
	struct port *p;

	clock_gettime(CLOCK_MONOTONIC, &msg->ts.host);

	for (p = clock_first_port(q->clock); p; p = LIST_NEXT(p, list)) {
		if (tc_blocked(q, p, msg)) {
			continue;
		}
		if (p->timestamping == TS_ONESTEP) {
			if (clock_is_prp(q->clock)) {
				if (!tc_prp_should_fwd(q, p, msg))
					continue;
				tc_prp_set_port_number_bits(q, p, msg, true);
				tc_prp_clear_resp_port_number_bits(q, msg);
			}
			if ((transport_send(p->trp, &p->fda, TRANS_GENERAL, msg)) <= 0) {
				p->errorCounter++;
				pr_err("tc failed to forward response on port %d", portnum(p));
				port_dispatch(p, EV_FAULT_DETECTED, 0);
			}
			continue;
		}
		tc_complete(q, p, msg, tmv_zero());
	}
	return 0;
}

int tc_fwd_sync(struct port *q, struct ptp_message *msg)
{
	struct ptp_message *fup = NULL;
	int err;

	if (q->timestamping >= TS_ONESTEP) {
		if (!one_step(msg))
			return tc_twostep_to_onestep_syfup(q, msg);
		else
			goto onestep;
	}

	if (one_step(msg)) {
		fup = msg_allocate();
		if (!fup) {
			return -1;
		}
		fup->header.tsmt               = FOLLOW_UP | (msg->header.tsmt & 0xf0);
		fup->header.ver                = msg->header.ver;
		fup->header.messageLength      = htons(sizeof(struct follow_up_msg));
		fup->header.domainNumber       = msg->header.domainNumber;
		fup->header.sourcePortIdentity = msg->header.sourcePortIdentity;
		fup->header.sequenceId         = msg->header.sequenceId;
		fup->header.logMessageInterval = msg->header.logMessageInterval;
		fup->follow_up.preciseOriginTimestamp = msg->sync.originTimestamp;
		msg->header.flagField[0]      |= TWO_STEP;
	}

 onestep:
	err = tc_fwd_event(q, msg);
	if (err) {
		return err;
	}
	if (fup) {
		err = tc_fwd_folup(q, fup);
		msg_put(fup);
	}
	return err;
}

int tc_ignore(struct port *p, struct ptp_message *m)
{
	struct ClockIdentity c1, c2;

	if (p->match_transport_specific &&
	    msg_transport_specific(m) != p->transportSpecific) {
		return 1;
	}
	if (pid_eq(&m->header.sourcePortIdentity, &p->portIdentity)) {
		return 1;
	}
	if (m->header.domainNumber != clock_domain_number(p->clock)) {
		return 1;
	}

	c1 = clock_identity(p->clock);
	c2 = m->header.sourcePortIdentity.clockIdentity;

	if (cid_eq(&c1, &c2)) {
		return 1;
	}
	return 0;
}

void tc_prune(struct port *q)
{
	struct timespec now;
	struct tc_txd *txd;

	clock_gettime(CLOCK_MONOTONIC, &now);

	while ((txd = TAILQ_FIRST(&q->tc_transmitted)) != NULL) {
		if (tc_current(txd->msg, now)) {
			break;
		}
		TAILQ_REMOVE(&q->tc_transmitted, txd, list);
		msg_put(txd->msg);
		tc_recycle(txd);
	}
}
