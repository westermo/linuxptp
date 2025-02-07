/**
 * @file red.c
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

#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <sys/queue.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <time.h>

#include "bmc.h"
#include "clock.h"
#include "fd.h"
#include "foreign.h"
#include "fsm.h"
#include "interface.h"
#include "msg.h"
#include "net_tstamp_cpy.h"
#include "port_private.h"
#include "tmv.h"
#include "transport.h"
#include "util.h"
#include "print.h"
#include "rtnl.h"
#include "red.h"
#include "red_private.h"
#include "transport_private.h"

static void red_port_p2p_transition(struct red_port *rp, enum port_state next);
static enum fsm_event red_switchover(struct red_port *from, enum port_state from_next);
static void red_dispatch_ports(struct port *p);
static void red_port_notify_event(struct red_port *rp, enum notification event);
static void red_port_set_socket_clk_type(struct red_port *rp, int clk_type);
static void red_hsr_swap_clock_mode(struct port *p);
static int red_port_fault_timeout(struct red_port *rp, int set);
static int red_set_delay_tmo(struct port *p);
static int red_set_sync_tx_tmo(struct port *p);

static bool red_is_boundary(struct port *p)
{
	return clock_type(p->clock) == CLOCK_TYPE_BOUNDARY;
}

static bool red_is_transparent(struct port *p)
{
	return clock_type(p->clock) == CLOCK_TYPE_P2P;
}

static void red_fds_swap(struct port *p)
{
	int tmp_efd = 0;
	int tmp_gfd = 0;
	tmp_efd = p->fda.fd[FD_EVENT];
	tmp_gfd = p->fda.fd[FD_GENERAL];
	p->fda.fd[FD_EVENT] = p->fda.fd[FD_EVENT_B];
	p->fda.fd[FD_GENERAL] = p->fda.fd[FD_GENERAL_B];
	p->fda.fd[FD_EVENT_B] = tmp_efd;
	p->fda.fd[FD_GENERAL_B] = tmp_gfd;
}

static bool red_is_a(struct red_port *rp)
{
	return rp == rp->upper->red_a;
}

static struct red_port *red_other_port(struct red_port *rp)
{
	return rp == rp->upper->red_a ? rp->upper->red_b : rp->upper->red_a;
}

/* static bool red_is_b(struct red_port *rp) */
/* { */
/* 	return rp == rp->upper->red_b; */
/* } */

static int red_anno_fd(struct red_port *rp)
{
	return red_is_a(rp) ? FD_ANNOUNCE_TIMER : FD_ANNOUNCE_TIMER_B;
}

static int red_rtnl_fd(struct red_port *rp)
{
	return red_is_a(rp) ? FD_RTNL : FD_RTNL_B;
}

static int red_event_fd(struct red_port *rp)
{
	return red_is_a(rp) ? FD_EVENT : FD_EVENT_B;
}

static bool red_port_is_slave(struct red_port *rp)
{
	return rp->state == PS_UNCALIBRATED || rp->state == PS_SLAVE;
}

static bool red_port_is_slave_backup(struct red_port *rp)
{
	return rp->state == PS_PASSIVE_SLAVE;
}

static void red_port_show_transition(struct red_port *rp, enum port_state state)
{
	pr_notice("%s: %s to %s", rp->log_name, ps_str[rp->state], ps_str[state]);
}

static bool red_port_up(struct red_port *rp)
{
	if (rp->link_status & LINK_UP && rp->state != PS_FAULTY && rp->state != PS_DISABLED)
		return true;
	return false;
}

static void red_port_set_state(struct red_port *rp, enum port_state state)
{
	if (rp->link_status & LINK_UP) {
		red_port_show_transition(rp, state);
		rp->state = state;
	} else if (rp->state != PS_FAULTY) {
		red_port_show_transition(rp, PS_FAULTY);
		rp->state = PS_FAULTY;
	}
}

static void red_link_status(void *ctx, int linkup, int ts_index)
{
	char ts_label[MAX_IFNAME_SIZE + 1] = {0};
	int link_state;
	const char *old_ts_label;
	struct red_port *rp = ctx;

	link_state = linkup ? LINK_UP : LINK_DOWN;
	if (rp->link_status & link_state) {
		rp->link_status = link_state;
	} else {
		rp->link_status = link_state | LINK_STATE_CHANGED;
		/* Update Interface speed information on Link up*/
		if (linkup) {
			interface_get_ifinfo(rp->iface);
		}

		pr_notice("%s: link %s", rp->log_name, linkup ? "up" : "down");
	}

	/* ts_label changed */
	old_ts_label = interface_label(rp->iface);
	if (if_indextoname(ts_index, ts_label) && strcmp(old_ts_label, ts_label)) {
		interface_set_label(rp->iface, ts_label);
		rp->link_status |= TS_LABEL_CHANGED;
		pr_notice("%s: ts label changed to %s", rp->log_name, ts_label);
	}

	/* The PHC index may change even with the same ts_label, e.g. after
	   failover with VLAN over bond. */
	interface_get_tsinfo(rp->iface);
}

static int red_port_link_status_get(struct red_port *rp)
{
	return rp->link_status & LINK_UP;
}

static int red_link_status_get(struct port *p)
{
	return (p->red_a->link_status & LINK_UP) && (p->red_b->link_status & LINK_UP);
}

static enum fsm_event red_port_link_status_event(struct red_port *rp)
{
	struct red_port *other = red_other_port(rp);
	int rp_up = rp->link_status == (LINK_UP | LINK_STATE_CHANGED);
	int rp_down = (rp->link_status == (LINK_DOWN | LINK_STATE_CHANGED)) || (rp->link_status & TS_LABEL_CHANGED);
	
	
	if (rp_up) {
		if (rp->state != PS_FAULTY)
			return EV_NONE;
		red_port_fault_timeout(rp, 0);
		rp->state = PS_INITIALIZING;
		if (rp->upper->state == PS_FAULTY)
			/* Triggers a red_dispatch() to clear FAULTY
			 * and initialize ports.
			 */
			return EV_FAULT_CLEARED;
		else
			red_dispatch_ports(rp->upper);
	} else if (rp_down) {
		red_port_p2p_transition(rp, PS_FAULTY);
		if (other->state == PS_FAULTY) {
			/* Triggers a red_dispatch() to set RED to
			 * FAULTY.
			*/
			return EV_FAULT_DETECTED;
		} else {
			red_dispatch_ports(rp->upper);
			return EV_NONE;
		}
	}
	return EV_NONE;
}

static void red_port_set_hw_path_delay(struct red_port *rp)
{
	Integer64 value;

	if (clock_is_prp(rp->clock) && red_is_boundary(rp->upper))
		return;

	/* Include ingr/egr latency for HW forwarded packets.
	 * Ideally they would be separate registers in HW, but right
	 * now they are not. */
	value = tmv_to_nanoseconds(rp->peer_delay)
		+ (rp->rx_timestamp_offset >> 16)
		+ (rp->tx_timestamp_offset >> 16);

	/* Just use the event file descriptor */
	port_write_hw_path_delay(rp->name, rp->upper->fda.fd[red_event_fd(rp)], value);
}

static int red_port_set_announce_tmo(struct red_port *rp)
{
	int anno_fd = red_is_a(rp) ? FD_ANNOUNCE_TIMER : FD_ANNOUNCE_TIMER_B;
	return set_tmo_random(rp->upper->fda.fd[anno_fd],
			      rp->upper->announceReceiptTimeout,
			      rp->upper->announce_span, rp->upper->logAnnounceInterval);
}

static int red_port_set_announce_tmo_onesec(struct red_port *rp)
{
	int anno_fd = red_is_a(rp) ? FD_ANNOUNCE_TIMER : FD_ANNOUNCE_TIMER_B;
	return set_tmo_lin(rp->upper->fda.fd[anno_fd], 1);
}

static int red_port_set_fault_timer_lin(struct red_port *rp, int seconds)
{
	int fault_fd = red_is_a(rp) ? FD_FAULT_RED_A : FD_FAULT_RED_B;
	return set_tmo_lin(rp->upper->fda.fd[fault_fd], seconds);
}

static int red_port_fault_timeout(struct red_port *rp, int set)
{
	if (!set) {
		pr_debug("clearing fault on %s", rp->log_name);
		return red_port_set_fault_timer_lin(rp, 0);
	}

	pr_debug("waiting %d seconds to clear fault on %s", 10, rp->log_name);
	// XXX: Fault length is hardcoded to 10
	return red_port_set_fault_timer_lin(rp, 10);
}

static enum fsm_event red_is_faulty(struct port *p)
{
	if (p->red_a->state == PS_FAULTY && p->red_b->state == PS_FAULTY)
		return EV_FAULT_DETECTED;
	return EV_NONE;
}

static void red_port_nrate_initialize(struct red_port *rp)
{
	int shift = rp->upper->freq_est_interval - rp->upper->logPdelayReqInterval;

	if (shift < 0)
		shift = 0;
	else if (shift >= sizeof(int) * 8) {
		shift = sizeof(int) * 8 - 1;
		pr_warning("freq_est_interval is too long");
	}

	/* We start in the 'incapable' state. */
	rp->pdr_missing = rp->upper->allowedLostResponses + 1;

	rp->peer_portid_valid = 0;

	rp->nrate.origin1 = tmv_zero();
	rp->nrate.ingress1 = tmv_zero();
	rp->nrate.max_count = (1U << shift);
	rp->nrate.count = 0;
	rp->nrate.ratio = 1.0;
	rp->nrate.ratio_valid = 0;
}

static void red_port_init_rtnl(struct red_port *rp)
{

	int rtnl_fd = red_rtnl_fd(rp);

	if (rp->upper->fda.fd[rtnl_fd] == -1) {
		rp->upper->fda.fd[rtnl_fd] = rtnl_open();
	}
	if (rp->upper->fda.fd[rtnl_fd] >= 0) {
		const char *ifname = interface_name(rp->iface);
		rtnl_link_query(rp->upper->fda.fd[rtnl_fd], ifname);
	}
}

static int red_port_initialize(struct red_port *rp)
{
	if (red_is_a(rp)) {
		if (transport_open(rp->trp, rp->iface, &rp->upper->fda, rp->upper->timestamping))
			goto no_tropen;

	} else {
		red_fds_swap(rp->upper);
		if (transport_open(rp->trp, rp->iface, &rp->upper->fda, rp->upper->timestamping)) {
			red_fds_swap(rp->upper);
			goto no_tropen;
		}
		red_fds_swap(rp->upper);
	}

	/* If current clock type is TC then we have to change since
	 * transport_open will have initialized as BC */
	if (red_is_boundary(rp->upper) && rp->upper->curr_clktype == HWTSTAMP_CLOCK_TYPE_TRANSPARENT_CLOCK) {
		red_port_set_socket_clk_type(rp, HWTSTAMP_CLOCK_TYPE_TRANSPARENT_CLOCK);
	}

	if (red_port_set_announce_tmo(rp)) {
		goto no_tmo;
	}
	red_port_init_rtnl(rp);
	red_port_nrate_initialize(rp);

	clock_fda_changed(rp->upper->clock);

	/* Reset timers. We've seen TX timestamps fail if done
	 * immediately after link goes up (even though timestamping
	 * should be configured). This delays the timers an extra
	 * second.
	 */
	red_set_delay_tmo(rp->upper);
	if (rp->upper->state == PS_MASTER || rp->upper->state == PS_GRAND_MASTER)
		red_set_sync_tx_tmo(rp->upper);
	return 0;
no_tmo:
	if (red_is_a(rp)) {
		transport_close(rp->trp, &rp->upper->fda);
	} else {
		red_fds_swap(rp->upper);
		transport_close(rp->trp, &rp->upper->fda);
		red_fds_swap(rp->upper);
	}
no_tropen:
	return -1;
}

int red_initialize(struct port *p)
{
	struct config *cfg = clock_config(p->clock);
	int fd[N_TIMER_FDS], i;

	p->multiple_seq_pdr_count  = 0;
	p->multiple_pdr_detected   = 0;
	p->last_fault_type         = FT_UNSPECIFIED;
	p->logMinDelayReqInterval  = config_get_int(cfg, p->name, "logMinDelayReqInterval");
	p->peerMeanPathDelay       = 0;
	p->initialLogAnnounceInterval = config_get_int(cfg, p->name, "logAnnounceInterval");
	p->logAnnounceInterval     = p->initialLogAnnounceInterval;
	p->inhibit_announce        = config_get_int(cfg, p->name, "inhibit_announce");
	p->ignore_source_id        = config_get_int(cfg, p->name, "ignore_source_id");
	p->announceReceiptTimeout  = config_get_int(cfg, p->name, "announceReceiptTimeout");
	p->syncReceiptTimeout      = config_get_int(cfg, p->name, "syncReceiptTimeout");
	p->transportSpecific       = config_get_int(cfg, p->name, "transportSpecific");
	p->transportSpecific     <<= 4;
	p->match_transport_specific = !config_get_int(cfg, p->name, "ignore_transport_specific");
	p->localPriority           = config_get_int(cfg, p->name, "G.8275.portDS.localPriority");
	p->initialLogSyncInterval  = config_get_int(cfg, p->name, "logSyncInterval");
	p->logSyncInterval         = p->initialLogSyncInterval;
	p->operLogSyncInterval     = config_get_int(cfg, p->name, "operLogSyncInterval");
	p->logMinPdelayReqInterval = config_get_int(cfg, p->name, "logMinPdelayReqInterval");
	p->logPdelayReqInterval    = p->logMinPdelayReqInterval;
	p->operLogPdelayReqInterval = config_get_int(cfg, p->name, "operLogPdelayReqInterval");
	p->neighborPropDelayThresh = config_get_int(cfg, p->name, "neighborPropDelayThresh");
	p->min_neighbor_prop_delay = config_get_int(cfg, p->name, "min_neighbor_prop_delay");
	p->delay_response_timeout  = config_get_int(cfg, p->name, "delay_response_timeout");
	p->iface_rate_tlv 	   = config_get_int(cfg, p->name, "interface_rate_tlv");

	if (config_get_int(cfg, p->name, "asCapable") == AS_CAPABLE_TRUE) {
		p->asCapable = ALWAYS_CAPABLE;
	} else {
		p->asCapable = NOT_CAPABLE;
	}

	p->inhibit_delay_req = config_get_int(cfg, p->name, "inhibit_delay_req");
	if (p->inhibit_delay_req && p->asCapable != ALWAYS_CAPABLE) {
		pr_err("inhibit_delay_req can only be set when asCapable == 'true'.");
		return -1;
	}
	if (port_delay_mechanism(p) == DM_NO_MECHANISM) {
		p->inhibit_delay_req = 1;
	}

	for (i = 0; i < N_TIMER_FDS; i++) {
		fd[i] = -1;
	}
	for (i = 0; i < N_TIMER_FDS; i++) {
		fd[i] = timerfd_create(CLOCK_MONOTONIC, 0);
		if (fd[i] < 0) {
			pr_err("timerfd_create: %s", strerror(errno));
			goto no_timers;
		}
	}

	for (i = 0; i < N_TIMER_FDS; i++) {
		p->fda.fd[FD_FIRST_TIMER + i] = fd[i];
	}

	if (red_port_initialize(p->red_a))
		goto no_tmo;
	if (red_port_initialize(p->red_b))
		goto no_tmo;

	clock_fda_changed(p->clock);

	memset(&p->redundant_bc_info, 0, sizeof(struct redundant_bc_info));

	return 0;

no_tmo:
no_timers:
	for (i = 0; i < N_TIMER_FDS; i++) {
		if (fd[i] >= 0)
			close(fd[i]);
	}
	return -1;
}

static void red_port_flush_peer_delay(struct red_port *rp)
{
	if (rp->peer_delay_req) {
		msg_put(rp->peer_delay_req);
		rp->peer_delay_req = NULL;
	}
	if (rp->peer_delay_resp) {
		msg_put(rp->peer_delay_resp);
		rp->peer_delay_resp = NULL;
	}
	if (rp->peer_delay_fup) {
		msg_put(rp->peer_delay_fup);
		rp->peer_delay_fup = NULL;
	}
}

static void red_port_clear_fda(struct red_port *rp)
{
	rp->upper->fda.fd[red_anno_fd(rp)] = -1;
}

static void red_clear_fda(struct port *p, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (i == FD_ANNOUNCE_TIMER || i == FD_ANNOUNCE_TIMER_B)
			continue;
		p->fda.fd[i] = -1;
	}
}

static void red_port_free_foreign_masters(struct red_port *rp)
{
	struct foreign_clock *fc;
	while ((fc = LIST_FIRST(&rp->foreign_masters)) != NULL) {
		LIST_REMOVE(fc, list);
		fc_clear(fc);
		free(fc);
	}
}

static void red_port_disable(struct red_port *rp)
{

	if (rp->state == PS_DISABLED || rp->state == PS_FAULTY)
		return;

	/* flush_last_sync(p); */
	/* flush_delay_req(p); */
	red_port_flush_peer_delay(rp);

	port_clr_tmo(rp->upper->fda.fd[red_anno_fd(rp)]);

	// XXX: Do we need to signal something? This does not feel like a perfect solution
	rp->upper->best = red_other_port(rp)->best;
	rp->best = NULL;
	red_port_free_foreign_masters(rp);
	
	if (red_is_a(rp)) {
		transport_close(rp->trp, &rp->upper->fda);
		rp->upper->fda.fd[FD_EVENT] = -1;
		rp->upper->fda.fd[FD_GENERAL] = -1;
	} else {
		red_fds_swap(rp->upper);
		transport_close(rp->trp, &rp->upper->fda);
		red_fds_swap(rp->upper);
		rp->upper->fda.fd[FD_EVENT_B] = -1;
		rp->upper->fda.fd[FD_GENERAL_B] = -1;
	}
	clock_fda_changed(rp->clock);
}

void red_disable(struct port *p)
{
	int i;

	p->best = NULL;
	
	for (i = 0; i < N_TIMER_FDS; i++) {
		if (i == FD_ANNOUNCE_TIMER || i == FD_ANNOUNCE_TIMER_B)
			continue;
		close(p->fda.fd[FD_FIRST_TIMER + i]);
	}

	/* Keep rtnl socket to get link status info. */
	red_port_disable(p->red_a);
	red_port_disable(p->red_b);

	red_clear_fda(p, FD_RTNL);
	red_clear_fda(p, FD_RTNL_B);
	clock_fda_changed(p->clock);

	port_clear_fda(p, FD_RTNL);
}
static int red_set_delay_tmo(struct port *p)
{
	return set_tmo_log(p->fda.fd[FD_DELAY_TIMER], 1,
			   p->logPdelayReqInterval);
}

static int red_set_qualification_tmo(struct port *p)
{
	return set_tmo_log(p->fda.fd[FD_QUALIFICATION_TIMER],
		       1+clock_steps_removed(p->clock), p->logAnnounceInterval);
}

static int red_set_sync_tx_tmo(struct port *p)
{
	return set_tmo_log(p->fda.fd[FD_SYNC_TX_TIMER], 1, p->logSyncInterval);
}

static int red_set_manno_tmo(struct port *p)
{
	return set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, p->logAnnounceInterval);
}

static void red_port_try_set_anno_tmo(struct red_port *rp)
{
	if (rp->state == PS_DISABLED || rp->state == PS_FAULTY)
		return;
	red_port_set_announce_tmo(rp);
}

static struct foreign_clock *red_port_compute_best(struct red_port *rp)
{
	int (*dscmp)(struct dataset *a, struct dataset *b);
	struct foreign_clock *fc;
	struct ptp_message *tmp;

	dscmp = clock_dscmp(rp->clock);
	rp->best = NULL;

	if (rp->upper->master_only)
		return rp->upper->best;

	LIST_FOREACH(fc, &rp->foreign_masters, list) {
		tmp = TAILQ_FIRST(&fc->messages);
		if (!tmp)
			continue;

		announce_to_dataset(tmp, rp->upper, &fc->dataset);

		fc_prune(fc);

		if (fc->n_messages < FOREIGN_MASTER_THRESHOLD)
			continue;

		if (!rp->best)
			rp->best = fc;
		else if (dscmp(&fc->dataset, &rp->best->dataset) > 0)
			rp->best = fc;
		else
			fc_clear(fc);
	}
	return rp->best;
}

struct foreign_clock *red_compute_best(struct port *p)
{
	int (*dscmp)(struct dataset *a, struct dataset *b);
	struct foreign_clock *best_a, *best_b;

	dscmp = clock_dscmp(p->clock);
	p->best = NULL;
	
	best_a = red_port_compute_best(p->red_a);
	best_b = red_port_compute_best(p->red_b);
	
	if (best_a && best_b) {
		if (dscmp(&best_a->dataset, &best_b->dataset) >= 0)
			p->best = best_a;
		else
			p->best = best_b;
	} else if (best_a) {
		p->best = best_a;
	} else {
		p->best = best_b;
	}

	return p->best;
}

static int red_state_update(struct port *p, enum fsm_event event, int mdiff)
{
	enum port_state next = p->state_machine(p->state, event, mdiff);

	/* The commented code below would only be relevant if we
	 * allowed redundant TC via HSR-PRP. Since we follow IEC
	 * 62439-3:2016, it is explicitly "not recommended and not
	 * further specified".
	 */
	/* TC should never be PASSIVE */
	/* if (red_is_transparent(p) && next == PS_PASSIVE) */
		/* next = PS_MASTER; */

	if (PS_FAULTY == next) {
		struct fault_interval i;
		fault_interval(p, last_fault_type(p), &i);
		if (red_link_status_get(p) && clear_fault_asap(&i)) {
			pr_notice("%s: clearing fault immediately", p->log_name);
			next = p->state_machine(next, EV_FAULT_CLEARED, 0);
		}
	}

	if (PS_INITIALIZING == next) {
		/*
		 * This is a special case. Since we initialize the
		 * port immediately, we can skip right to listening
		 * state if all goes well.
		 */
		if (port_is_enabled(p)) {
			red_disable(p);
		}
		if (red_initialize(p)) {
			event = EV_FAULT_DETECTED;
		} else {
			event = EV_INIT_COMPLETE;
		}
		next = p->state_machine(next, event, 0);
		p->red_a->state = PS_INITIALIZING;
		p->red_b->state = PS_INITIALIZING;
	}

	if (mdiff) {
		p->unicast_state_dirty = true;
	}
	if (next != p->state) {
		port_show_transition(p, next, event);
		p->state = next;
		/* port_notify_event(p, NOTIFY_PORT_STATE); */
		p->unicast_state_dirty = true;
		return 1;
	}

	return 0;
}

static struct red_port *red_compute_slave_port(struct port *p)
{
	struct foreign_clock *fc = red_compute_best(p);

	if (p->red_a->best && fc == p->red_a->best) {
		return p->red_a;
	} else if (p->red_b->best && fc == p->red_b->best) {
		return p->red_b;
	}
	return NULL;
}

static void red_port_p2p_transition(struct red_port *rp, enum port_state next)
{
	if (rp->state == next)
		return;

	port_clr_tmo(rp->upper->fda.fd[red_anno_fd(rp)]);
	rp->anno_timed_out = false;

	switch (next) {
	case PS_INITIALIZING:
		break;
	case PS_FAULTY:
	case PS_DISABLED:
		red_port_disable(rp);
		break;
	case PS_LISTENING:
		red_port_try_set_anno_tmo(rp);
		break;
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
		break;
	case PS_PASSIVE:
		red_port_try_set_anno_tmo(rp);
		break;
	case PS_UNCALIBRATED:
	case PS_PASSIVE_SLAVE:
		red_port_flush_peer_delay(rp);
		/* fall through */
	case PS_SLAVE:
		red_port_try_set_anno_tmo(rp);
		break;
	};
	red_port_set_state(rp, next);
	red_port_notify_event(rp, NOTIFY_PORT_STATE);
}

static void red_port_fault(struct red_port *rp)
{
	red_port_p2p_transition(rp, PS_FAULTY);
	red_port_fault_timeout(rp, 1);
}

static void red_p2p_transition(struct port *p, enum port_state next)
{
	port_clr_tmo(p->fda.fd[FD_SYNC_RX_TIMER]);
	/* Leave FD_DELAY_TIMER running. */
	port_clr_tmo(p->fda.fd[FD_QUALIFICATION_TIMER]);
	port_clr_tmo(p->fda.fd[FD_MANNO_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);

	switch (next) {
	case PS_INITIALIZING:
		break;
	case PS_FAULTY:
	case PS_DISABLED:
		/* Should only happen if both RED ports are FAULTY and we trigger
		 * dispatch on EV_FAULT_DETECTED */
		red_disable(p);
		break;
	case PS_LISTENING:
		red_set_delay_tmo(p);
		break;
	case PS_PRE_MASTER:
		red_set_qualification_tmo(p);
		memset(&p->redundant_bc_info, 0, sizeof(struct redundant_bc_info));
		break;
	case PS_MASTER:
	case PS_GRAND_MASTER:
		if (red_is_boundary(p)) {
			if (!p->inhibit_announce) {
				set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, -10); /*~1ms*/
			}
			red_set_sync_tx_tmo(p);
		}
		memset(&p->redundant_bc_info, 0, sizeof(struct redundant_bc_info));
		break;
	case PS_PASSIVE:
		break;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		memset(&p->redundant_bc_info, 0, sizeof(struct redundant_bc_info));
		break;
	case PS_PASSIVE_SLAVE:
		break;
	};
}

static int cmp_timespec(struct timespec a, struct timespec b)
{
	if (a.tv_sec > b.tv_sec)
		return 1;
	else if (b.tv_sec > a.tv_sec)
		return -1;
	else if (a.tv_nsec > b.tv_nsec)
		return 1;
	else if (b.tv_nsec > a.tv_nsec)
		return -1;
	else
		return 0;
}

/* Other active BC exists in ring. Go to PASSIVE */
static enum fsm_event red_active_bc_exists(struct port *p, enum fsm_event event)
{
	struct timespec last_sync = p->redundant_bc_info.last_sync;
	struct timespec last_anno = p->redundant_bc_info.last_anno;
	struct dataset *clock_best_ds = clock_best_foreign(p->clock);
	struct dataset *red_best_ds = &p->best->dataset;
	struct timespec now;

	if (red_is_transparent(p))
		return event;
	if (event != EV_QUALIFICATION_TIMEOUT_EXPIRES)
		return event;

	/* Condition to enter PASSIVE after PRE_MASTER
	 * 1. There is another MASTER on the ring. (A)
	 * 2. The same MASTER has recently transmitted Sync (within 2 seconds)
	 *    and Announce. Meaning there is a master actively
	 *    transmitting. (B, C, D)
	 * 3. The Clock (i.e. interlink port) has better
	 *    quality than on the ring. Don't compare PID to
	 *    avoid overriding based on fallback identity. (E)
	 * 4. The active master has the same GM identity. (F)
	 * 5. The most recent Sync is that of the active ring master. (G)
	 *
	 * The checks below will exit if any of the above conditions are false.
	 */
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	last_sync.tv_sec += 2;
	last_anno.tv_sec += 2;

	if (!red_best_ds)
		return event; // A
	if (cmp_timespec(last_sync, now) < 0)
		return event; // B
	if (cmp_timespec(last_anno, now) < 0)
		return event; // C
	if(!pid_eq(&p->redundant_bc_info.sync_pid, &p->redundant_bc_info.anno_pid))
		return event; // D
	if(!(dscmp_no_id(red_best_ds, clock_best_foreign(p->clock)) >= B_BETTER))
		return event; // E
	if (!cid_eq(&red_best_ds->identity, &clock_best_ds->identity))
		return event; // F
	if (!pid_eq(&p->redundant_bc_info.sync_pid, &red_best_ds->sender))
		return event; // G

	return EV_RS_PASSIVE;
}

static void red_dispatch(struct port *p, enum fsm_event event, int mdiff)
{
	event = red_active_bc_exists(p, event);

	if (!red_state_update(p, event, mdiff)) {
		return;
	}

	if (p->delayMechanism == DM_P2P) {
		red_p2p_transition(p, p->state);
	} else {
		pr_err("RED E2E not implemented");
		return;
	}

	red_dispatch_ports(p);
	red_hsr_swap_clock_mode(p);
}

static enum fsm_event red_port_sync_anno_timer(struct red_port *rp, int fd_index)
{
	int anno_fd = red_anno_fd(rp);

	pr_debug("%s: %s timeout", rp->log_name,
		 fd_index == FD_SYNC_RX_TIMER ? "rx sync" : "announce");
	if (rp->best) {
		fc_clear(rp->best);
	}

	if (fd_index == FD_SYNC_RX_TIMER) {
		rp->upper->service_stats.sync_timeout++;
	} else {
		rp->upper->service_stats.announce_timeout++;
	}

	/* Clear out the event returned by poll(). It is only cleared
	 * in port_*_transition(). But, when BMCA == 'noop', there is no
	 * state transition. So, it won't be cleared anywhere else.
	 */
	if (rp->upper->bmca == BMCA_NOOP) {
		port_clr_tmo(rp->upper->fda.fd[FD_SYNC_RX_TIMER]);
	}

	if (rp->upper->inhibit_announce) {
		port_clr_tmo(rp->upper->fda.fd[anno_fd]);
	} else {
		red_port_set_announce_tmo(rp);
	}

	if (rp->upper->inhibit_announce) {
		return EV_NONE;
	}

	return EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES;
}

static void red_stats_inc_rx(struct port *p, const struct ptp_message *msg)
{
	p->stats.rxMsgType[msg_type(msg)]++;
}

static void red_stats_inc_tx(struct port *p, const struct ptp_message *msg)
{
	p->stats.txMsgType[msg_type(msg)]++;
}

/* Traffic duplicated in HW should only be sent on one port.
 * Prioritize port A if it's available, otherwise fall back to B.
 */
static struct red_port *red_get_active_port(struct port *p)
{
	struct red_port *rp = p->red_a;
	if (p->red_a->state == PS_FAULTY)
		rp = p->red_b;
	return rp;
}

static int red_port_send(struct red_port *rp, enum transport_event event, struct ptp_message *msg)
{
	int cnt;

	if (red_is_a(rp)) {
		cnt = transport_send(rp->trp, &rp->upper->fda, event, msg);
	} else {
		/* Temporarily swap around FDs */
		red_fds_swap(rp->upper);
		cnt = transport_send(rp->trp, &rp->upper->fda, event, msg);
		red_fds_swap(rp->upper);
	}
	return cnt;
}
static int red_port_peer(struct red_port *rp, enum transport_event event, struct ptp_message *msg)
{
	int cnt;

	if (red_is_a(rp)) {
		cnt = transport_peer(rp->trp, &rp->upper->fda, event, msg);
	} else {
		red_fds_swap(rp->upper);
		cnt = transport_peer(rp->trp, &rp->upper->fda, event, msg);
		red_fds_swap(rp->upper);
	}
	return cnt;
}

static int red_port_prepare_and_send(struct red_port *rp, struct ptp_message *msg,
				     enum transport_event event)
{
	int cnt;

	if (msg_pre_send(msg)) {
		return -1;
	}

	cnt = red_port_send(rp, event, msg);
	if (cnt <= 0) {
		rp->upper->errorCounter++;
		return -1;
	}
	red_stats_inc_tx(rp->upper, msg);
	if (msg_sots_valid(msg)) {
		ts_add(&msg->hwts.ts, rp->tx_timestamp_offset);
	}
	return 0;
}

int red_prepare_and_send(struct port *p, struct ptp_message *msg,
			 enum transport_event event)
{
	return red_port_prepare_and_send(red_get_active_port(p), msg, event);
}

int red_send(struct port *p, struct ptp_message *msg)
{
	struct red_port *rp = red_get_active_port(p);
	int cnt;

	cnt = red_port_send(rp, TRANS_GENERAL, msg);
	if (cnt <= 0) {
		return -1;
	}
	red_stats_inc_tx(p, msg);
	return 0;
}

static int red_peer_prepare_and_send(struct red_port *rp, struct ptp_message *msg,
				     enum transport_event event)
{
	int cnt;
	if (msg_pre_send(msg)) {
		return -1;
	}

	cnt = red_port_peer(rp, event, msg);
	if (cnt <= 0) {
		return -1;
	}
	/* port_stats_inc_tx(p, msg); */
	if (msg_sots_valid(msg)) {
		ts_add(&msg->hwts.ts, rp->tx_timestamp_offset);
	}
	return 0;
}

static int red_tx_sync(struct port *p, struct address *dst, uint16_t sequence_id)
{
	struct ptp_message *msg = NULL, *fup = NULL;
	int err, event;

	switch (p->timestamping) {
	// TODO: 2-step not supported. Using TS_SOFTWARE for testing.
	// Just sends empty Syncs.
	case TS_SOFTWARE:
	/* case TS_LEGACY_HW: */
	/* case TS_HARDWARE: */
		event = TRANS_EVENT;
		break;
	case TS_ONESTEP:
		event = TRANS_ONESTEP;
		break;
	case TS_P2P1STEP:
		event = TRANS_P2P1STEP;
		break;
	default:
		return -1;
	}

	if (p->inhibit_multicast_service && !dst) {
		return 0;
	}

	msg = msg_allocate();
	if (!msg) {
		return -1;
	}
	fup = msg_allocate();
	if (!fup) {
		msg_put(msg);
		return -1;
	}

	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = SYNC | p->transportSpecific;
	msg->header.ver                = ptp_hdr_ver;
	msg->header.messageLength      = sizeof(struct sync_msg);
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = sequence_id;
	msg->header.logMessageInterval = p->logSyncInterval;

	if (p->timestamping != TS_ONESTEP && p->timestamping != TS_P2P1STEP) {
		msg->header.flagField[0] |= TWO_STEP;
	} else {
		/* It seems to assume corrections should be done in hardware
		 * for onestep sync. Let's try in software.
		 */
		/* XXX: Note: Since packets are duplicated in HW, it
		* doesn't make sense if the ports have different offsets.
		* But we need to use from one of the ports.
		*/
		msg->header.correction = red_get_active_port(p)->tx_timestamp_offset;
	}

	if (dst) {
		msg->address = *dst;
		msg->header.flagField[0] |= UNICAST;
		msg->header.logMessageInterval = 0x7f;
	}
	err = red_prepare_and_send(p, msg, event);
	if (err) {
		pr_err("%s: send sync failed", p->log_name);
		goto out;
	}

	if (p->timestamping == TS_ONESTEP || p->timestamping == TS_P2P1STEP) {
		goto out;
	} else if (msg_sots_missing(msg)) {
		pr_err("missing timestamp on transmitted sync");
		err = -1;
		goto out;
	}

	/*
	 * Send the follow up message right away.
	 */
	/* fup->hwts.type = p->timestamping; */

	/* fup->header.tsmt               = FOLLOW_UP | p->transportSpecific; */
	/* fup->header.ver                = ptp_hdr_ver; */
	/* fup->header.messageLength      = sizeof(struct follow_up_msg); */
	/* fup->header.domainNumber       = clock_domain_number(p->clock); */
	/* fup->header.sourcePortIdentity = p->portIdentity; */
	/* fup->header.sequenceId         = sequence_id; */
	/* fup->header.logMessageInterval = p->logSyncInterval; */

	/* fup->follow_up.preciseOriginTimestamp = tmv_to_Timestamp(msg->hwts.ts); */

	/* if (dst) { */
	/* 	fup->address = *dst; */
	/* 	fup->header.flagField[0] |= UNICAST; */
	/* } */
	/* /\* if (p->follow_up_info && follow_up_info_append(fup)) { *\/ */
	/* /\* 	pr_err("%s: append fup info failed", p->log_name); *\/ */
	/* /\* 	err = -1; *\/ */
	/* /\* 	goto out; *\/ */
	/* /\* } *\/ */

	/* err = red_port_prepare_and_send(red_get_active_port(p), fup, TRANS_GENERAL); */
	/* if (err) { */
	/* 	pr_err("%s: send follow up failed", p->log_name); */
	/* } */
out:
	if (msg)
		msg_put(msg);
	if (fup)
		msg_put(fup);
	return err;
}

static int red_tx_announce(struct port *p, struct address *dst, uint16_t sequence_id)
{
	struct timePropertiesDS tp = clock_time_properties(p->clock);
	struct parent_ds *dad = clock_parent_ds(p->clock);
	struct ptp_message *msg;
	int err;

	if (p->inhibit_multicast_service && !dst) {
		return 0;
	}
	if (!port_capable(p)) {
		return 0;
	}
	msg = msg_allocate();
	if (!msg) {
		return -1;
	}

	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = ANNOUNCE | p->transportSpecific;
	msg->header.ver                = ptp_hdr_ver;
	msg->header.messageLength      = sizeof(struct announce_msg);
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = sequence_id;
	msg->header.logMessageInterval = p->logAnnounceInterval;

	msg->header.flagField[1] = tp.flags;

	if (dst) {
		msg->address = *dst;
		msg->header.flagField[0] |= UNICAST;
	}
	msg->announce.currentUtcOffset        = tp.currentUtcOffset;
	msg->announce.grandmasterPriority1    = dad->pds.grandmasterPriority1;
	msg->announce.grandmasterClockQuality = dad->pds.grandmasterClockQuality;
	msg->announce.grandmasterPriority2    = dad->pds.grandmasterPriority2;
	msg->announce.grandmasterIdentity     = dad->pds.grandmasterIdentity;
	msg->announce.stepsRemoved            = clock_steps_removed(p->clock);
	msg->announce.timeSource              = tp.timeSource;

	/* if (ieee_c37_238_append(p, msg)) { */
	/* 	pr_err("%s: append power profile failed", p->log_name); */
	/* } */
	if (clock_append_timezones(p->clock, msg)) {
		pr_err("%s: append time zones failed", p->log_name);
	}
	
	err = red_prepare_and_send(p, msg, TRANS_GENERAL);
	if (err) {
		pr_err("%s: send announce failed", p->log_name);
	}
	msg_put(msg);
	return err;
}


static int red_port_pdelay_request(struct red_port *rp)
{
	struct ptp_message *msg;
	int err;

	/* Time to send a new request, forget current pdelay resp and fup */
	if (rp->peer_delay_resp) {
		msg_put(rp->peer_delay_resp);
		rp->peer_delay_resp = NULL;
	}
	if (rp->peer_delay_fup) {
		msg_put(rp->peer_delay_fup);
		rp->peer_delay_fup = NULL;
	}
	
	if (!red_port_up(rp))
		return 0;

	msg = msg_allocate();
	if (!msg) {
		return -1;
	}

	msg->hwts.type = rp->upper->timestamping;

	msg->header.tsmt               = PDELAY_REQ | rp->upper->transportSpecific;
	msg->header.ver                = ptp_hdr_ver;
	msg->header.messageLength      = sizeof(struct pdelay_req_msg);
	msg->header.domainNumber       = clock_domain_number(rp->clock);
	msg->header.correction         = -rp->asymmetry;
	msg->header.sourcePortIdentity = rp->portIdentity;
	msg->header.sequenceId         = rp->seqnum.delayreq++;
	msg->header.logMessageInterval = rp->upper->logPdelayReqInterval;

	err = red_peer_prepare_and_send(rp, msg, TRANS_EVENT);
	if (err) {
		pr_err("%s: send peer delay request failed", rp->log_name);
		goto out;
	}
	if (msg_sots_missing(msg)) {
		pr_err("missing timestamp on transmitted peer delay request");
		goto out;
	}

	if (rp->peer_delay_req) {
		msg_put(rp->peer_delay_req);
	}
	rp->peer_delay_req = msg;
	return 0;
out:
	msg_put(msg);
	return -1;
}

static int red_ignore(struct port *p, struct ptp_message *m)
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

static void red_port_synchronize(struct red_port *rp,
			     uint16_t seqid,
			     tmv_t ingress_ts,
			     struct timestamp origin_ts,
			     Integer64 correction1, Integer64 correction2,
			     Integer8 sync_interval)
{
	enum servo_state state; //, last_state;
	tmv_t t1, t1c, t2, c1, c2;

	// TODO: Handle sync RX tmo ???
	/* if (port_set_sync_rx_tmo(p) < 0) { */
	/* 	pr_err("Failed to set sync rx timeout timer: %s", strerror(errno)); */
	/* } */

	t1 = timestamp_to_tmv(origin_ts);
	t2 = ingress_ts;
	c1 = correction_to_tmv(correction1);
	c2 = correction_to_tmv(correction2);
	t1c = tmv_add(t1, tmv_add(c1, c2));

	switch (rp->state) {
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		monitor_sync(rp->upper->slave_event_monitor,
			     clock_parent_identity(rp->clock), seqid,
			     t1, tmv_add(c1, c2), t2);
		break;
	default:
		break;
	}

	/* last_state = clock_servo_state(rp->clock); */
	state = clock_synchronize(rp->clock, t2, t1c);
	switch (state) {
	case SERVO_UNLOCKED:
		port_dispatch(rp->upper, EV_SYNCHRONIZATION_FAULT, 0);
		if (servo_offset_threshold(clock_servo(rp->clock)) != 0 &&
		    sync_interval != rp->upper->initialLogSyncInterval) {
			rp->upper->logPdelayReqInterval = rp->upper->logMinPdelayReqInterval;
			rp->upper->logSyncInterval = rp->upper->initialLogSyncInterval;
		}
		break;
	case SERVO_JUMP:
		port_dispatch(rp->upper, EV_SYNCHRONIZATION_FAULT, 0);
		if (rp->peer_delay_req) {
			msg_put(rp->peer_delay_req);
			rp->peer_delay_req = NULL;
		}
		break;
	case SERVO_LOCKED:
		port_dispatch(rp->upper, EV_MASTER_CLOCK_SELECTED, 0);
		break;
	case SERVO_LOCKED_STABLE:
		/* message_interval_request(p, last_state, sync_interval); */
		port_dispatch(rp->upper, EV_MASTER_CLOCK_SELECTED, 0);
		break;
	}
}

static void red_port_process_sync(struct red_port *rp, struct ptp_message *m)
{
	switch (rp->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		return;
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
	case PS_PASSIVE_SLAVE:
		clock_gettime(CLOCK_MONOTONIC_RAW, &rp->upper->redundant_bc_info.last_sync);
		rp->upper->redundant_bc_info.sync_pid = m->header.sourcePortIdentity;
		return;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		clock_gettime(CLOCK_MONOTONIC_RAW, &rp->upper->redundant_bc_info.last_sync);
		rp->upper->redundant_bc_info.sync_pid = m->header.sourcePortIdentity;
		break;
	}

	if (check_source_identity(rp->upper, m)) {
		return;
	}

	if (!msg_unicast(m) &&
	    m->header.logMessageInterval != rp->upper->log_sync_interval) {
		if (m->header.logMessageInterval < -10 ||
		    m->header.logMessageInterval > 22) {
			pl_info(300, "%s: ignore bogus sync interval 2^%d",
				rp->log_name, m->header.logMessageInterval);
		} else {
			rp->upper->log_sync_interval = m->header.logMessageInterval;
			clock_sync_interval(rp->clock, rp->upper->log_sync_interval);
		}
	}

	m->header.correction += rp->asymmetry;

	if (one_step(m)) {
		red_port_synchronize(rp, m->header.sequenceId,
				     m->hwts.ts, m->ts.pdu,
				     m->header.correction, 0,
				     m->header.logMessageInterval);
		/* flush_last_sync(p); */
		return;
	}
	/* TODO: Support 2-step reception */
	pr_err("RED: Received 2-step Sync. Not supported");

	/* if (p->syfu == SF_HAVE_FUP && */
	/*     fup_sync_ok(p->last_syncfup, m) && */
	/*     p->last_syncfup->header.sequenceId == m->header.sequenceId) { */
	/* 	event = SYNC_MATCH; */
	/* } else { */
	/* 	event = SYNC_MISMATCH; */
	/* } */
	/* port_syfufsm(p, event, m); */
}

static int red_port_process_pdelay_req(struct red_port *rp, struct ptp_message *m)
{
	struct ptp_message *rsp, *fup;
	enum transport_event event;
	int err;

	switch (rp->upper->timestamping) {
	case TS_SOFTWARE:
	case TS_LEGACY_HW:
	case TS_HARDWARE:
	case TS_ONESTEP:
		event = TRANS_EVENT;
		break;
	case TS_P2P1STEP:
		event = TRANS_P2P1STEP;
		break;
	default:
		return -1;
	}

	/* Dummy frame for hardware that can only do p2p1step but needs to do
	 * normal one-step (two-step for P2P). Send a dummy frame to make it
	 * look like two-step. The time calculations should still be the same.
	 */
	if (rp->dummy_pdelay_resp_fup)
		event = TRANS_P2P1STEP;

	if (rp->upper->delayMechanism == DM_E2E) {
		pr_warning("%s: pdelay_req on E2E port", rp->log_name);
		return 0;
	}

	if (rp->peer_portid_valid) {
		if (!pid_eq(&rp->peer_portid, &m->header.sourcePortIdentity)) {
			pr_err("%s: received pdelay_req msg with "
				"unexpected peer port id %s",
				rp->log_name,
				pid2str(&m->header.sourcePortIdentity));
			rp->peer_portid_valid = 0;
			/* port_capable(p); */
		}
	} else {
		rp->peer_portid_valid = 1;
		rp->peer_portid = m->header.sourcePortIdentity;
		pr_debug("%s: peer port id set to %s", rp->log_name,
			pid2str(&rp->peer_portid));
	}

	rsp = msg_allocate();
	if (!rsp) {
		return -1;
	}

	fup = msg_allocate();
	if (!fup) {
		msg_put(rsp);
		return -1;
	}

	rsp->hwts.type = rp->upper->timestamping;

	rsp->header.tsmt               = PDELAY_RESP | rp->upper->transportSpecific;
	rsp->header.ver                = ptp_hdr_ver;
	rsp->header.messageLength      = sizeof(struct pdelay_resp_msg);
	rsp->header.domainNumber       = m->header.domainNumber;
	rsp->header.sourcePortIdentity = rp->portIdentity;
	rsp->header.sequenceId         = m->header.sequenceId;
	rsp->header.logMessageInterval = 0x7f;

	/*
	 * NB - We do not have any fraction nanoseconds for the correction
	 * fields, neither in the response or the follow up.
	 */
	if (rp->upper->timestamping == TS_P2P1STEP) {
		rsp->header.correction = m->header.correction;
		rsp->header.correction += rp->tx_timestamp_offset;
		rsp->header.correction += rp->rx_timestamp_offset;
		rsp->header.reserved2  = m->header.reserved2;
	} else if (rp->dummy_pdelay_resp_fup) {
		rsp->header.correction += rp->tx_timestamp_offset;
		rsp->header.correction += rp->rx_timestamp_offset;
		rsp->header.reserved2  = m->header.reserved2;
		rsp->header.flagField[0] |= TWO_STEP;
	} else {
		rsp->header.flagField[0] |= TWO_STEP;
		rsp->pdelay_resp.requestReceiptTimestamp =
			tmv_to_Timestamp(m->hwts.ts);
	}
	rsp->pdelay_resp.requestingPortIdentity = m->header.sourcePortIdentity;

	err = red_peer_prepare_and_send(rp, rsp, event);
	if (err) {
		pr_err("%s: send peer delay response failed", rp->log_name);
		goto out;
	}
	if (rp->upper->timestamping == TS_P2P1STEP) {
		goto out;
	} else if (!rp->dummy_pdelay_resp_fup && msg_sots_missing(rsp)) {
		pr_err("missing timestamp on transmitted peer delay response");
		err = -1;
		goto out;
	}

	/*
	 * Send the follow up message right away.
	 */
	fup->hwts.type = rp->upper->timestamping;

	fup->header.tsmt               = PDELAY_RESP_FOLLOW_UP | rp->upper->transportSpecific;
	fup->header.ver                = ptp_hdr_ver;
	fup->header.messageLength      = sizeof(struct pdelay_resp_fup_msg);
	fup->header.domainNumber       = m->header.domainNumber;
	fup->header.correction         = m->header.correction;
	fup->header.sourcePortIdentity = rp->portIdentity;
	fup->header.sequenceId         = m->header.sequenceId;
	fup->header.logMessageInterval = 0x7f;

	fup->pdelay_resp_fup.requestingPortIdentity = m->header.sourcePortIdentity;

	if (!rp->dummy_pdelay_resp_fup)
		fup->pdelay_resp_fup.responseOriginTimestamp =
			tmv_to_Timestamp(rsp->hwts.ts);

	if (msg_unicast(m)) {
		fup->address = m->address;
		fup->header.flagField[0] |= UNICAST;
	}

	err = red_peer_prepare_and_send(rp, fup, TRANS_GENERAL);
	if (err) {
		pr_err("%s: send pdelay_resp_fup failed", rp->log_name);
	}
out:
	msg_put(rsp);
	msg_put(fup);
	return err;
}

static void red_port_peer_delay(struct red_port *rp)
{
	tmv_t c1, c2, t1, t2, t3, t3c, t4;
	struct ptp_message *req = rp->peer_delay_req;
	struct ptp_message *rsp = rp->peer_delay_resp;
	struct ptp_message *fup = rp->peer_delay_fup;

	/* Check for response, validate port and sequence number. */
	if (!rsp)
		return;

	if (!pid_eq(&rsp->pdelay_resp.requestingPortIdentity, &rp->portIdentity))
		return;

	if (rsp->header.sequenceId != ntohs(req->header.sequenceId))
		return;

	t1 = req->hwts.ts;
	t4 = rsp->hwts.ts;
	c1 = correction_to_tmv(rsp->header.correction + rp->asymmetry);

	/* Process one-step response immediately. */
	if (one_step(rsp)) {
		t2 = tmv_zero();
		t3 = tmv_zero();
		c2 = tmv_zero();
		goto calc;
	}

	/* Check for follow up, validate port and sequence number. */

	if (!fup)
		return;

	if (!pid_eq(&fup->pdelay_resp_fup.requestingPortIdentity, &rp->portIdentity))
		return;

	if (fup->header.sequenceId != rsp->header.sequenceId)
		return;

	if (!source_pid_eq(fup, rsp))
		return;

	/* Process follow up response. */
	t2 = timestamp_to_tmv(rsp->ts.pdu);
	t3 = timestamp_to_tmv(fup->ts.pdu);
	c2 = correction_to_tmv(fup->header.correction);
calc:
	/* 802.1AS specifies the peer delay computation differently than 1588. Do
	 * the 802.1AS computation if transportSpecific matches 802.1AS profile. */
	if (rp->upper->transportSpecific == TS_IEEE_8021AS) {
		t3c = tmv_add(t3, tmv_sub(c2, c1));
	} else {
		t3c = tmv_add(t3, tmv_add(c1, c2));
	}

	/* if (p->follow_up_info) */
		/* port_nrate_calculate(p, t3c, t4); */

	tsproc_set_clock_rate_ratio(rp->tsproc, rp->nrate.ratio *
				    clock_rate_ratio(rp->clock));
	tsproc_up_ts(rp->tsproc, t1, t2);
	tsproc_down_ts(rp->tsproc, t3c, t4);
	if (tsproc_update_delay(rp->tsproc, &rp->peer_delay))
		return;

	rp->peerMeanPathDelay = tmv_to_TimeInterval(rp->peer_delay);

	red_port_set_hw_path_delay(rp);

	if (rp->state == PS_UNCALIBRATED || rp->state == PS_SLAVE) {
		clock_peer_delay(rp->clock, rp->peer_delay, t1, t2,
				 rp->nrate.ratio);
	}

	msg_put(rp->peer_delay_req);
	rp->peer_delay_req = NULL;
}

static int red_port_process_pdelay_resp(struct red_port *rp, struct ptp_message *m)
{
	/* if (p->peer_delay_resp) { */
        /*         if (!p->multiple_pdr_detected) { */
        /*                 pr_err("%s: multiple peer responses", p->log_name); */
        /*                 p->multiple_pdr_detected = 1; */
        /*                 p->multiple_seq_pdr_count++; */
        /*         } */
        /*         if (p->multiple_seq_pdr_count > p->allowedLostResponses) { */
        /*                 p->last_fault_type = FT_BAD_PEER_NETWORK; */
        /*                 return -1; */
        /*         } */
        /* } */

	if (!rp->peer_delay_req) {
		pr_err("%s: rogue peer delay response", rp->log_name);
		/* Let's not trigger an error on this. It seems to
		 * happen fairly regularly on state changes since it
		 * flushes the pdelay.
		 */
		return 0;
	}
	if (rp->peer_portid_valid) {
		if (!pid_eq(&rp->peer_portid, &m->header.sourcePortIdentity)) {
			pr_err("%s: received pdelay_resp msg with "
				"unexpected peer port id %s",
				rp->log_name,
				pid2str(&m->header.sourcePortIdentity));
			rp->peer_portid_valid = 0;
		}
	} else {
		rp->peer_portid_valid = 1;
		rp->peer_portid = m->header.sourcePortIdentity;
		pr_debug("%s: peer port id set to %s", rp->log_name,
			pid2str(&rp->peer_portid));
	}

	if (rp->peer_delay_resp) {
		msg_put(rp->peer_delay_resp);
	}
	msg_get(m);
	rp->peer_delay_resp = m;
	red_port_peer_delay(rp);
	return 0;
}

static void red_port_process_pdelay_resp_fup(struct red_port *rp, struct ptp_message *m)
{
	if (!rp->peer_delay_req) {
		return;
	}

	if (rp->peer_delay_fup) {
		msg_put(rp->peer_delay_fup);
	}

	msg_get(m);
	rp->peer_delay_fup = m;
	red_port_peer_delay(rp);
}

static int red_port_add_foreign_master(struct red_port *rp, struct ptp_message *m)
{
	struct foreign_clock *fc;
	struct ptp_message *tmp;
	int broke_threshold = 0, diff = 0;

	LIST_FOREACH(fc, &rp->foreign_masters, list) {
		if (msg_source_equal(m, fc)) {
			break;
		}
	}
	if (!fc) {
		pr_notice("%s: new foreign master %s", rp->log_name,
			pid2str(&m->header.sourcePortIdentity));

		fc = malloc(sizeof(*fc));
		if (!fc) {
			pr_err("low memory, failed to add foreign master");
			return 0;
		}
		memset(fc, 0, sizeof(*fc));
		TAILQ_INIT(&fc->messages);
		LIST_INSERT_HEAD(&rp->foreign_masters, fc, list);
		fc->port = rp->upper;
		fc->dataset.sender = m->header.sourcePortIdentity;
		/* We do not count this first message, see 9.5.3(b) */
		return 0;
	}

	/*
	 * If this message breaks the threshold, that is an important change.
	 */
	fc_prune(fc);
	if (FOREIGN_MASTER_THRESHOLD - 1 == fc->n_messages) {
		broke_threshold = 1;
	}

	/*
	 * Okay, go ahead and add this announcement.
	 */
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);

	/*
	 * Test if this announcement contains changed information.
	 */
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		diff = announce_compare(m, tmp);
	}

	return broke_threshold || diff;
}

static int red_port_update_current_master(struct red_port *rp, struct ptp_message *m)
{
	struct foreign_clock *fc = rp->best;
	struct ptp_message *tmp;
	struct timePropertiesDS tds;

	if (!msg_source_equal(m, fc))
		return red_port_add_foreign_master(rp, m);

	if (rp->state != PS_PASSIVE) {
		tds.currentUtcOffset = m->announce.currentUtcOffset;
		tds.flags = m->header.flagField[1];
		tds.timeSource = m->announce.timeSource;
		clock_update_time_properties(rp->clock, tds);
	}

	rp->anno_timed_out = false;
	red_port_set_announce_tmo(rp);
	fc_prune(fc);
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		return announce_compare(m, tmp);
	}
	return 0;
}

static int red_port_process_announce(struct red_port *rp, struct ptp_message *m)
{
	int result = 0;

	if (m->announce.stepsRemoved >= clock_max_steps_removed(rp->clock)) {
		return result;
	}

	if (m->announce.grandmasterClockQuality.clockClass >
		clock_get_clock_class_threshold(rp->clock)) {
		pl_err(60, "%s: Master clock quality received is "
			"greater than configured, ignoring master!",
			rp->log_name);
		return result;
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &rp->upper->redundant_bc_info.last_anno);
	rp->upper->redundant_bc_info.anno_pid = m->header.sourcePortIdentity;

	switch (rp->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		break;
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
		result = red_port_add_foreign_master(rp, m);
		break;
	case PS_PASSIVE:
	case PS_UNCALIBRATED:
	case PS_PASSIVE_SLAVE:
	case PS_SLAVE:
		result = red_port_update_current_master(rp, m);
		break;
	}
	return result;
}

static bool red_port_is_master_backup(struct red_port *rp)
{
	return rp->state == PS_PASSIVE;
}

static enum fsm_event red_port_timeout_backup(struct red_port *rp)
{
	struct red_port *other = red_other_port(rp);
	
	/* Timeout must have happened on both to trigger takeover, or
	 * if the other port is down.
	 */
	if (other->anno_timed_out || !red_port_up(other))
		return EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES;

	rp->anno_timed_out = true;
	return EV_NONE;
}

static enum fsm_event red_port_timeout_slave(struct red_port *rp)
{
	struct red_port *other = red_other_port(rp);

	/* If it's the second timeout in a row. Wait until both ports
	 * have timed out. Then we try to assume GM role. This is to
	 * prevent trying to assume Master role if there are redundant
	 * BCs in the network.
	 */
	if (rp->anno_timed_out) {
		if (other->anno_timed_out || !red_port_up(other))
			return EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES;
		return red_switchover(rp, PS_PASSIVE_SLAVE);
	}

	rp->anno_timed_out = true;
	/* Set a one second timeout. This is so we don't wait another
	* 3-4 seconds for next timeout in case a cable broken. But we
	* still need some extra time for a Backup Master to take over
	* and send out new Announce messages.
	*/
	red_port_set_announce_tmo_onesec(rp);
	return EV_NONE;
}

static enum fsm_event red_port_anno_tmo(struct red_port *rp, int fd_index)
{
	enum fsm_event event;
	event = red_port_sync_anno_timer(rp, fd_index);
	if (event == EV_NONE)
		return event;

	if (red_port_is_master_backup(rp))
		return red_port_timeout_backup(rp);
	if (red_port_is_slave(rp) || red_port_is_slave_backup(rp))
		return red_port_timeout_slave(rp);

	return event;
}

static enum fsm_event red_event(struct port *p, int fd_index)
{
	enum fsm_event event = EV_NONE;
	struct ptp_message *msg;
	int cnt, fd = p->fda.fd[fd_index], err;
	int faults = 0;

	switch (fd_index) {
	case FD_ANNOUNCE_TIMER:
	case FD_SYNC_RX_TIMER:
		return red_port_anno_tmo(p->red_a, fd_index);

	case FD_ANNOUNCE_TIMER_B:
		return red_port_anno_tmo(p->red_b, fd_index);

	case FD_DELAY_TIMER:
		pr_debug("%s: delay timeout", p->log_name);
		red_set_delay_tmo(p);
		/* delay_req_prune(p); */
		p->service_stats.delay_timeout++;
		if (red_port_pdelay_request(p->red_a)) {
			red_port_fault(p->red_a);
			faults++;
		}
		if (red_port_pdelay_request(p->red_b)) {
			red_port_fault(p->red_b);
			faults++;
		}
		if (faults) {
			/* If both ports are faulty, return ev fault.
			 * If only one is faulty, dispatch and update states.
			 */
			if (red_is_faulty(p))
				return EV_FAULT_DETECTED;
			else if (faults)
				red_dispatch_ports(p);
		}
		return EV_NONE;

	case FD_QUALIFICATION_TIMER:
		pr_debug("%s: qualification timeout", p->log_name);
		p->service_stats.qualification_timeout++;
		return EV_QUALIFICATION_TIMEOUT_EXPIRES;

	case FD_MANNO_TIMER:
		pr_debug("%s: master tx announce timeout", p->log_name);
		red_set_manno_tmo(p);
		p->service_stats.master_announce_timeout++;
		clock_update_leap_status(p->clock);
		return red_tx_announce(p, NULL, p->seqnum.announce++) ?
			EV_FAULT_DETECTED : EV_NONE;

	case FD_SYNC_TX_TIMER:
		pr_debug("%s: master sync timeout", p->log_name);
		red_set_sync_tx_tmo(p);
		p->service_stats.master_sync_timeout++;
		return red_tx_sync(p, NULL, p->seqnum.sync++) ?
			EV_FAULT_DETECTED : EV_NONE;

	case FD_RTNL:
		pr_debug("%s: received link status notification", p->red_a->log_name);
		rtnl_link_status(fd, p->red_a->name, red_link_status, p->red_a);
		return red_port_link_status_event(p->red_a);

	case FD_RTNL_B:
		pr_debug("%s: received link status notification", p->red_b->log_name);
		rtnl_link_status(fd, p->red_b->name, red_link_status, p->red_b);
		return red_port_link_status_event(p->red_b);

	case FD_FAULT_RED_A:
		red_port_fault_timeout(p->red_a, 0);
		if (!red_port_link_status_get(p->red_a))
			return EV_NONE;
		p->red_a->state = PS_INITIALIZING;
		if (p->state == PS_FAULTY)
			return EV_FAULT_CLEARED;
		else
			red_dispatch_ports(p);
		return EV_NONE;

	case FD_FAULT_RED_B:
		red_port_fault_timeout(p->red_b, 0);
		if (!red_port_link_status_get(p->red_b))
			return EV_NONE;
		p->red_b->state = PS_INITIALIZING;
		if (p->state == PS_FAULTY)
			return EV_FAULT_CLEARED;
		else
			red_dispatch_ports(p);
		return EV_NONE;
	}

	struct red_port *rp = NULL;
	
	if (fd_index == FD_EVENT || fd_index == FD_GENERAL)
		rp = p->red_a;
	else if (fd_index == FD_EVENT_B || fd_index == FD_GENERAL_B)
		rp = p->red_b;

	msg = msg_allocate();
	if (!msg)
		return EV_FAULT_DETECTED;

	msg->hwts.type = p->timestamping;

	cnt = transport_recv(rp->trp, fd, msg);
	if (cnt < 0) {
		pr_err("%s: recv message failed", rp->log_name);
		msg_put(msg);
		return EV_FAULT_DETECTED;
	}
	err = msg_post_recv(msg, cnt);
	if (err) {
		switch (err) {
		case -EBADMSG:
			pr_err("%s: bad message", rp->log_name);
			break;
		case -EPROTO:
			pr_debug("%s: ignoring message", rp->log_name);
			break;
		}
		msg_put(msg);
		return EV_NONE;
	}
	red_stats_inc_rx(p, msg);
	if (red_ignore(p, msg)) {
		msg_put(msg);
		return EV_NONE;
	}
	if (msg_sots_missing(msg) &&
	    !(p->timestamping == TS_P2P1STEP && msg_type(msg) == PDELAY_REQ)) {
		pr_err("%s: received %s without timestamp",
		       rp->log_name, msg_type_string(msg_type(msg)));
		msg_put(msg);
		return EV_NONE;
	}
	if (msg_sots_valid(msg)) {
		ts_add(&msg->hwts.ts, -rp->rx_timestamp_offset);
		if (rp->state == PS_SLAVE) {
			clock_check_ts(p->clock,
				       tmv_to_nanoseconds(msg->hwts.ts));
		}
	}

	switch (msg_type(msg)) {
	case SYNC:
		red_port_process_sync(rp, msg);
		break;
	case PDELAY_REQ:
		if (red_port_process_pdelay_req(rp, msg)) {
			red_port_fault(rp);
			if (red_is_faulty(rp->upper))
				event = EV_FAULT_DETECTED;
		}
		break;
	case PDELAY_RESP:
		if (red_port_process_pdelay_resp(rp, msg)) {
			red_port_fault(rp);
			if (red_is_faulty(rp->upper))
				event = EV_FAULT_DETECTED;
		}
		break;
	/* case FOLLOW_UP: */
	/* 	red_process_follow_up(p, msg); */
	/* 	break; */
	case PDELAY_RESP_FOLLOW_UP:
		red_port_process_pdelay_resp_fup(rp, msg);
		break;
	case ANNOUNCE:
		if (red_port_process_announce(rp, msg))
			event = EV_STATE_DECISION_EVENT;
		break;
	case MANAGEMENT:
		// TODO: Management does send/forward replies on RED ports
		if (clock_manage(p->clock, p, msg))
			event = EV_STATE_DECISION_EVENT;
		break;
	}

	msg_put(msg);
	return event;
}

static int red_set_phc(struct config *cfg, const char *phc_device,
		int phc_index, struct port *p, struct red_port *rp)
{
	if (!interface_tsinfo_valid(rp->iface)) {
		pr_warning("%s: get_ts_info not supported", rp->log_name);
	} else if (rp->phc_index >= 0 &&
		   rp->phc_index != interface_phc_index(rp->iface)) {
		if (p->jbod) {
			pr_warning("%s: just a bunch of devices", rp->log_name);
			rp->phc_index = interface_phc_index(rp->iface);
		/* } else if (phc_device) { */
		/* 	pr_warning("%s: taking %s from the command line, " */
		/* 		   "not the attached ptp%d", rp->log_name, */
		/* 		   phc_device, interface_phc_index(rp->iface)); */
		/* 	rp->phc_index = phc_index; */
		/* 	p->phc_from_cmdline = 1; */
		} else {
			pr_err("%s: PHC device mismatch", rp->log_name);
			pr_err("%s: /dev/ptp%d requested, ptp%d attached",
			       rp->log_name, phc_index,
			       interface_phc_index(rp->iface));
			return 1;
		}
	}
	return 0;
}

struct port *red_open(const char *phc_device,
		      int phc_index,
		      enum timestamp_type timestamping,
		      int number,
		      struct interface *iface_a,
		      struct interface *iface_b,
		      struct clock *clock)
{
	enum clock_type type = clock_type(clock);
	struct config *cfg = clock_config(clock);
	struct port *p = malloc(sizeof(*p));
	struct interface *interface;
	struct red_port *red_a = NULL;
	struct red_port *red_b = NULL;
	int err, i;

	memset(p, 0, sizeof(*p));
	TAILQ_INIT(&p->tc_transmitted);

	if (!p) {
		return NULL;
	}
	
	red_a = calloc(1, sizeof(struct red_port));
	if (!red_a)
		goto err_red;

	red_b = calloc(1, sizeof(struct red_port));
	if (!red_b)
		goto err_red;

	p->red_a = red_a;
	p->red_b = red_b;
	red_a->iface = iface_a;
	red_b->iface = iface_b;
	
	interface = interface_create("RED");
	if (!interface)
		goto err_port;

	p->iface = interface;
	p->name = interface_name(interface);
	if (asprintf(&p->log_name, "port %d (%s)", number, p->name) == -1) {
		p->log_name = NULL;
		goto err_iface;
	}

	red_a->name = interface_name(iface_a);
	if (asprintf(&red_a->log_name, "RED (%s)", red_a->name) == -1) {
		red_a->log_name = NULL;
		goto err_log_name;
	}
	red_b->name = interface_name(iface_b);
	if (asprintf(&red_b->log_name, "RED (%s)", red_b->name) == -1) {
		red_b->log_name = NULL;
		goto err_log_name;
	}

	switch (type) {
	case CLOCK_TYPE_ORDINARY:
	case CLOCK_TYPE_BOUNDARY:
	case CLOCK_TYPE_P2P:
		p->dispatch = red_dispatch;
		p->event    = red_event;
		break;
	case CLOCK_TYPE_E2E:
	case CLOCK_TYPE_MANAGEMENT:
		pr_err("Unsupported clock type for RED interface");
		goto err_log_name;
	}

	if (timestamping == TS_ONESTEP) {
		p->dummy_pdelay_resp_fup =
			config_get_int(cfg, interface_name(iface_a), "dummy_pdelay_resp_fup");
		red_a->dummy_pdelay_resp_fup =
			config_get_int(cfg, interface_name(iface_a), "dummy_pdelay_resp_fup");
		red_b->dummy_pdelay_resp_fup =
			config_get_int(cfg, interface_name(iface_b), "dummy_pdelay_resp_fup");
	}

	/* p->phc_index = config_get_int(cfg, interface_name(interface), "phc_index"); */
	/* if (p->phc_index < 0) */
	/* 	p->phc_index = phc_index; */
	p->jbod = config_get_int(cfg, interface_name(iface_a), "boundary_clock_jbod");
	p->master_only = config_get_int(cfg, interface_name(iface_a), "serverOnly");
	p->bmca = config_get_int(cfg, interface_name(iface_a), "BMCA");
	red_a->trp = NULL;
	red_b->trp = NULL;
	red_a->trp = transport_create(cfg, config_get_int(cfg,
			      interface_name(iface_a), "network_transport"));
	if (!red_a->trp)
		goto err_transport;
	red_b->trp = transport_create(cfg, config_get_int(cfg,
			      interface_name(iface_b), "network_transport"));
	if (!red_b->trp)
		goto err_transport;

	if (p->bmca == BMCA_NOOP && !port_is_uds(p)) {
		/* Assume we use normal FSM -- Casper */
		pr_err("Unsupported BMCA mode for RED interface");
		goto err_transport;
	} else {
		p->state_machine = clock_slave_only(clock) ? ptp_slave_fsm : ptp_fsm;
	}
	
	red_a->phc_index = config_get_int(cfg, interface_name(iface_a), "phc_index");
	if (red_a->phc_index < 0)
		red_a->phc_index = phc_index;
	red_b->phc_index = config_get_int(cfg, interface_name(iface_b), "phc_index");
	if (red_b->phc_index < 0)
		red_b->phc_index = phc_index;
	err = red_set_phc(cfg, phc_device, phc_index, p, red_a);
	err |= red_set_phc(cfg, phc_device, phc_index, p, red_b);
	if (err)
		goto err_transport;
	p->phc_index = -1;

	p->announce_span = 1;
	p->freq_est_interval = config_get_int(cfg, p->name, "freq_est_interval");
	red_a->asymmetry = config_get_int(cfg, red_a->name, "delayAsymmetry");
	red_b->asymmetry = config_get_int(cfg, red_b->name, "delayAsymmetry");
	red_a->rx_timestamp_offset = config_get_int(cfg, red_a->name, "ingressLatency");
	red_a->rx_timestamp_offset <<= 16;
	red_a->tx_timestamp_offset = config_get_int(cfg, red_a->name, "egressLatency");
	red_a->tx_timestamp_offset <<= 16;
	red_b->rx_timestamp_offset = config_get_int(cfg, red_b->name, "ingressLatency");
	red_b->rx_timestamp_offset <<= 16;
	red_b->tx_timestamp_offset = config_get_int(cfg, red_b->name, "egressLatency");
	red_b->tx_timestamp_offset <<= 16;

	p->link_status = LINK_UP;
	red_a->link_status = LINK_UP;
	red_b->link_status = LINK_UP;

	p->clock = clock;
	red_a->clock = clock;
	red_b->clock = clock;
	red_a->upper = p;
	red_b->upper = p;

	p->timestamping = timestamping;
	p->portIdentity.clockIdentity = clock_identity(clock);
	p->red_a->portIdentity.clockIdentity = clock_identity(clock);
	p->red_b->portIdentity.clockIdentity = clock_identity(clock);
	p->portIdentity.portNumber = number;
	p->red_a->portIdentity.portNumber = number;
	p->red_b->portIdentity.portNumber = number + 1;
	p->state = PS_INITIALIZING;
	red_a->state = PS_INITIALIZING;
	red_b->state = PS_INITIALIZING;
	p->delayMechanism = config_get_int(cfg, p->name, "delay_mechanism");
	p->versionNumber = PTP_MAJOR_VERSION;
	/* p->pwr.version = */
	/* 	config_get_int(cfg, p->name, "power_profile.version"); */
	/* p->pwr.grandmasterID = */
	/* 	config_get_int(cfg, p->name, "power_profile.grandmasterID"); */
	/* p->pwr.grandmasterTimeInaccuracy = */
	/* 	config_get_int(cfg, p->name, "power_profile.2011.grandmasterTimeInaccuracy"); */
	/* p->pwr.networkTimeInaccuracy = */
	/* 	config_get_int(cfg, p->name, "power_profile.2011.networkTimeInaccuracy"); */
	/* p->pwr.totalTimeInaccuracy = */
	/* 	config_get_int(cfg, p->name, "power_profile.2017.totalTimeInaccuracy"); */
	p->slave_event_monitor = clock_slave_monitor(clock);
	p->allowedLostResponses = config_get_int(cfg, p->name, "allowedLostResponses");

	/* Set fault timeouts to a default value */
	for (i = 0; i < FT_CNT; i++) {
		red_a->flt_interval_pertype[i].type = FTMO_LOG2_SECONDS;
		red_a->flt_interval_pertype[i].val = 4;
	}
	red_a->flt_interval_pertype[FT_BAD_PEER_NETWORK].type = FTMO_LINEAR_SECONDS;
	red_a->flt_interval_pertype[FT_BAD_PEER_NETWORK].val =
		config_get_int(cfg, red_a->name, "fault_badpeernet_interval");

	red_a->flt_interval_pertype[FT_UNSPECIFIED].val =
		config_get_int(cfg, red_a->name, "fault_reset_interval");

	/* Set fault timeouts to a default value */
	for (i = 0; i < FT_CNT; i++) {
		red_b->flt_interval_pertype[i].type = FTMO_LOG2_SECONDS;
		red_b->flt_interval_pertype[i].val = 4;
	}
	red_b->flt_interval_pertype[FT_BAD_PEER_NETWORK].type = FTMO_LINEAR_SECONDS;
	red_b->flt_interval_pertype[FT_BAD_PEER_NETWORK].val =
		config_get_int(cfg, red_b->name, "fault_badpeernet_interval");

	red_b->flt_interval_pertype[FT_UNSPECIFIED].val =
		config_get_int(cfg, red_b->name, "fault_reset_interval");

	red_b->tsproc = NULL;
	red_a->tsproc = tsproc_create(config_get_int(cfg, red_a->name, "tsproc_mode"),
				  config_get_int(cfg, red_a->name, "delay_filter"),
				  config_get_int(cfg, red_a->name, "delay_filter_length"));
	if (!red_a->tsproc) {
		pr_err("Failed to create time stamp processor");
		goto err_transport;
	}
	red_a->nrate.ratio = 1.0;

	red_b->tsproc = tsproc_create(config_get_int(cfg, red_b->name, "tsproc_mode"),
				  config_get_int(cfg, red_b->name, "delay_filter"),
				  config_get_int(cfg, red_b->name, "delay_filter_length"));
	if (!red_b->tsproc) {
		pr_err("Failed to create time stamp processor");
		goto err_tsproc;
	}
	red_b->nrate.ratio = 1.0;

	red_clear_fda(p, N_POLLFD);
	red_port_clear_fda(p->red_a);
	red_port_clear_fda(p->red_b);
	p->fault_fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (p->fault_fd < 0) {
		pr_err("timerfd_create failed: %m");
		goto err_tsproc;
	}

	p->egress_vlan_tagged = config_get_int(cfg, NULL, "egress_vlan.tagged");
	if (p->egress_vlan_tagged) {
		p->egress_vlan_id = config_get_int(cfg, NULL, "egress_vlan.id");
		p->egress_vlan_prio = config_get_int(cfg, NULL, "egress_vlan.prio");
	}
	p->errorCounter = 0;

	if (red_is_boundary(p))
		p->curr_clktype = HWTSTAMP_CLOCK_TYPE_BOUNDARY_CLOCK;
	else
		p->curr_clktype = HWTSTAMP_CLOCK_TYPE_TRANSPARENT_CLOCK;

	return p;

err_tsproc:
	if (red_a->tsproc)
		tsproc_destroy(red_a->tsproc);
	if (red_b->tsproc)
		tsproc_destroy(red_b->tsproc);
err_transport:
	if (red_a->trp)
		transport_destroy(red_a->trp);
	if (red_b->trp)
		transport_destroy(red_b->trp);
err_log_name:
	if (p->log_name)
		free(p->log_name);
	if (red_a->log_name)
		free(red_a->log_name);
	if (red_b->log_name)
		free(red_b->log_name);
err_iface:
	free(interface);
err_red:
	if (red_a)
		free(red_a);
	if (red_b)
		free(red_b);
	if (iface_a)
		free(iface_a);
	if (iface_b)
		free(iface_b);
err_port:
	free(p);
	return NULL;
}

void red_close(struct port *p)
{
	if (port_is_enabled(p)) {
		red_disable(p);
	}

	if (p->fda.fd[FD_RTNL] >= 0) {
		rtnl_close(p->fda.fd[FD_RTNL]);
	}
	if (p->fda.fd[FD_RTNL_B] >= 0) {
		rtnl_close(p->fda.fd[FD_RTNL]);
	}

	interface_destroy(p->iface);
	red_port_flush_peer_delay(p->red_a);
	red_port_flush_peer_delay(p->red_b);
	transport_destroy(p->red_a->trp);
	transport_destroy(p->red_b->trp);
	tsproc_destroy(p->red_a->tsproc);
	tsproc_destroy(p->red_b->tsproc);
	free(p->red_a->log_name);
	free(p->red_b->log_name);
	free(p->red_a);
	free(p->red_b);
	if (p->fault_fd >= 0) {
		close(p->fault_fd);
	}
	free(p->log_name);
	free(p);
}

static void red_switch_phc(struct port *p, int phc_index)
{
	struct red_port *rp = phc_index == p->red_a->phc_index ? p->red_a : p->red_b;

	if (p->phc_index == phc_index)
		return;

	if (p->jbod && phc_index >= 0 ) {
		p->phc_index = phc_index;
		if (clock_switch_phc_keep_servo(p->clock, p->phc_index)) {
			p->last_fault_type = FT_SWITCH_PHC;
			port_dispatch(p, EV_FAULT_DETECTED, 0);
		}
		clock_sync_interval(p->clock, p->log_sync_interval);
		struct tsproc *tsp = rp->tsproc;
		clock_peer_delay(p->clock, rp->peer_delay,
				 tsproc_get_t1(tsp), tsproc_get_t2(tsp), rp->nrate.ratio);
	}
}

static enum fsm_event red_switchover(struct red_port *from, enum port_state from_next)
{
	struct red_port *other = red_other_port(from);
	enum port_state prev = from->state;

	if (red_port_is_slave(from) && red_port_is_slave_backup(other)) {
		red_port_p2p_transition(from, from_next);
		red_port_p2p_transition(other, prev);
		red_switch_phc(from->upper, other->phc_index);
	}
	return EV_NONE;
}

static void red_dispatch_ports(struct port *p)
{
	struct red_port *slave = red_compute_slave_port(p);
	struct red_port *rpa = p->red_a;
	struct red_port *rpb = p->red_b;
	enum port_state next_a = PS_FAULTY;
	enum port_state next_b = PS_FAULTY;
	int phc_index = p->phc_index;
	
	if (rpa->state == PS_INITIALIZING && p->state != PS_FAULTY && p->state != PS_DISABLED) {
		red_port_initialize(rpa);
	}
	if (rpb->state == PS_INITIALIZING && p->state != PS_FAULTY && p->state != PS_DISABLED) {
		red_port_initialize(rpb);
	}
	
	/* red_port's have their settings updated later in red_port_p2p_transition() */
	switch (p->state) {
	case PS_INITIALIZING:
		break;
	case PS_FAULTY:
	case PS_DISABLED:
		next_a = p->state;
		next_b = p->state;
		break;
	case PS_LISTENING:
		next_a = p->state;
		next_b = p->state;
		break;
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
		next_a = p->state;
		next_b = p->state;
		break;
	case PS_PASSIVE:
		next_a = p->state;
		next_b = p->state;
		break;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		if (red_port_up(rpa) && red_port_up(rpb)) {
			next_a = slave == rpa ? p->state : PS_PASSIVE_SLAVE;
			next_b = slave == rpb ? p->state : PS_PASSIVE_SLAVE;
			phc_index = slave == rpa ? rpa->phc_index : rpb->phc_index;
			/* Don't swap if one port is already in UNCALIBRATED/SLAVE */
			if (next_a == p->state && red_port_is_slave(rpb)) {
				next_a = PS_PASSIVE_SLAVE;
				next_b = p->state;
				phc_index = rpb->phc_index;
			} else if (next_b == p->state && red_port_is_slave(rpa)) {
				next_a = p->state;
				next_b = PS_PASSIVE_SLAVE;
				phc_index = rpa->phc_index;
			}
		} else if (red_port_up(rpa)) {
			next_a = p->state;
			phc_index = rpa->phc_index;
		} else if (red_port_up(rpb)) {
			next_b = p->state;
			phc_index = rpb->phc_index;
		} else {
			pr_err("RED interface should not be up when both ports are down");
			red_dispatch(p, EV_FAULT_DETECTED, 0);
			return;
		}
		break;
	case PS_PASSIVE_SLAVE:
		pr_err("RED interface should never be PASSIVE_SLAVE");
		red_dispatch(p, EV_FAULT_DETECTED, 0);
		return;
	};

	rpa->anno_timed_out = false;
	rpb->anno_timed_out = false;
        if (red_port_up(rpa))
		red_port_p2p_transition(rpa, next_a);
        if (red_port_up(rpb))
		red_port_p2p_transition(rpb, next_b);

	if (p->state == PS_UNCALIBRATED || p->state == PS_SLAVE)
		red_switch_phc(p, phc_index);
}

static const Octet profile_id_drr[] = {0x00, 0x1B, 0x19, 0x00, 0x01, 0x00};
static const Octet profile_id_p2p[] = {0x00, 0x1B, 0x19, 0x00, 0x02, 0x00};
static const Octet profile_id_8275_1[] = {0x00, 0x19, 0xA7, 0x01, 0x02, 0x03};
static const Octet profile_id_8275_2[] = {0x00, 0x19, 0xA7, 0x02, 0x01, 0x02};

static int red_port_management_fill_response(struct red_port *rp,
					     struct ptp_message *rsp, int id)
{
	struct ieee_c37_238_settings_np *pwr;
	struct transparentClockPortDS *tcpds;
	struct port_service_stats_np *pssn;
	struct mgmt_clock_description *cd;
	struct management_tlv_datum *mtd;
	struct clock_description *desc;
	struct port_properties_np *ppn;
	struct port_hwclock_np *phn;
	struct management_tlv *tlv;
	struct port_stats_np *psn;
	/* struct foreign_clock *fc; */
	struct port_ds_np *pdsnp;
	struct tlv_extra *extra;
	/* struct PortIdentity pid; */
	const char *ts_label;
	struct portDS *pds;
	uint16_t u16;
	uint8_t *buf;
	int datalen;
	struct port *target = rp->upper;

	extra = tlv_extra_alloc();
	if (!extra) {
		pr_err("failed to allocate TLV descriptor");
		return 0;
	}
	extra->tlv = (struct TLV *) rsp->management.suffix;

	tlv = (struct management_tlv *) rsp->management.suffix;
	tlv->type = TLV_MANAGEMENT;
	tlv->id = id;

	switch (id) {
	case MID_NULL_MANAGEMENT:
		datalen = 0;
		break;
	case MID_CLOCK_DESCRIPTION:
		cd = &extra->cd;
		buf = tlv->data;
		cd->clockType = (UInteger16 *) buf;
		buf += sizeof(*cd->clockType);
		*cd->clockType = clock_type(target->clock);
		cd->physicalLayerProtocol = (struct PTPText *) buf;
		switch(transport_type(rp->trp)) {
		case TRANS_UDP_IPV4:
		case TRANS_UDP_IPV6:
		case TRANS_IEEE_802_3:
			ptp_text_set(cd->physicalLayerProtocol, "IEEE 802.3");
			break;
		default:
			ptp_text_set(cd->physicalLayerProtocol, NULL);
			break;
		}
		buf += sizeof(struct PTPText) + cd->physicalLayerProtocol->length;

		cd->physicalAddress = (struct PhysicalAddress *) buf;
		u16 = transport_physical_addr(rp->trp,
                                              cd->physicalAddress->address);
		memcpy(&cd->physicalAddress->length, &u16, 2);
		buf += sizeof(struct PhysicalAddress) + u16;

		cd->protocolAddress = (struct PortAddress *) buf;
		u16 = transport_type(rp->trp);
		memcpy(&cd->protocolAddress->networkProtocol, &u16, 2);
		u16 = transport_protocol_addr(rp->trp,
                                              cd->protocolAddress->address);
		memcpy(&cd->protocolAddress->addressLength, &u16, 2);
		buf += sizeof(struct PortAddress) + u16;

		desc = clock_description(target->clock);
		cd->manufacturerIdentity = buf;
		memcpy(cd->manufacturerIdentity,
                       desc->manufacturerIdentity, OUI_LEN);
		buf += OUI_LEN;
		*(buf++) = 0; /* reserved */

		cd->productDescription = (struct PTPText *) buf;
		ptp_text_copy(cd->productDescription, &desc->productDescription);
		buf += sizeof(struct PTPText) + cd->productDescription->length;

		cd->revisionData = (struct PTPText *) buf;
		ptp_text_copy(cd->revisionData, &desc->revisionData);
		buf += sizeof(struct PTPText) + cd->revisionData->length;

		cd->userDescription = (struct PTPText *) buf;
		ptp_text_copy(cd->userDescription, &desc->userDescription);
		buf += sizeof(struct PTPText) + cd->userDescription->length;

		if (target->delayMechanism == DM_P2P) {
			memcpy(buf, profile_id_p2p, PROFILE_ID_LEN);
		} else {
			struct config *cfg = clock_config(target->clock);
			if (config_get_int(cfg, NULL, "dataset_comparison") ==
			    DS_CMP_G8275) {
				if (transport_type(rp->trp) == TRANS_IEEE_802_3) {
					memcpy(buf, profile_id_8275_1, PROFILE_ID_LEN);
				} else {
					memcpy(buf, profile_id_8275_2, PROFILE_ID_LEN);
				}
			} else {
				memcpy(buf, profile_id_drr, PROFILE_ID_LEN);
			}
		}
		buf += PROFILE_ID_LEN;
		datalen = buf - tlv->data;
		break;
	case MID_PORT_DATA_SET:
		pds = (struct portDS *) tlv->data;
		pds->portIdentity            = rp->portIdentity;
		if (target->state == PS_GRAND_MASTER) {
			pds->portState = PS_MASTER;
		} else {
			pds->portState = rp->state;
		}
		pds->logMinDelayReqInterval  = target->logMinDelayReqInterval;
		pds->peerMeanPathDelay       = rp->peerMeanPathDelay;
		pds->logAnnounceInterval     = target->logAnnounceInterval;
		pds->announceReceiptTimeout  = target->announceReceiptTimeout;
		pds->logSyncInterval         = target->logSyncInterval;
		if (target->delayMechanism) {
			pds->delayMechanism = target->delayMechanism;
		} else {
			pds->delayMechanism = DM_E2E;
		}
		pds->logMinPdelayReqInterval = target->logMinPdelayReqInterval;
		pds->versionNumber           = target->versionNumber;
		datalen = sizeof(*pds);
		if (clock_is_hsr_or_prp(target->clock)) {
			pds->iec62439_ds.networkProtocol = transport_type(rp->trp);
			pds->iec62439_ds.portEnabled = 1;
			pds->iec62439_ds.dlyAsymmetry = 0;
			pds->iec62439_ds.profileId = PROFILE_SET_L2P2P;
			pds->iec62439_ds.vlanEnable = target->egress_vlan_tagged;
			pds->iec62439_ds.vlanId = target->egress_vlan_id;
			pds->iec62439_ds.vlanPrio = target->egress_vlan_prio;
			pds->iec62439_ds.twoStepFlag = 0; /* We don't support twostep for HSR/PRP for now */

			/* XXX: Not sure if other devices uses the
			 * same definition for their clockId (based on
			 * MAC), but ptp4l hides the transport layer from
			 * the PTP layer so this is the best we can do
			 * for now.
			 */
			memcpy(pds->iec62439_ds.peerIdentity, target->peer_portid.clockIdentity.id, 3);
			memcpy(&pds->iec62439_ds.peerIdentity[3], &target->peer_portid.clockIdentity.id[5], 3);

			pds->iec62439_ds.prpPairedPort = red_other_port(rp)->portIdentity.portNumber;
			if (clock_type(target->clock) == CLOCK_TYPE_BOUNDARY)
				pds->iec62439_ds.prpAttachment = PORT_TYPE_DABC;
			else
				pds->iec62439_ds.prpAttachment = PORT_TYPE_DATC;
			pds->iec62439_ds.errorCounter = target->errorCounter;
			pds->iec62439_ds.peerDelayLim = 100; /* Defined in IEC 62439-3 */
			datalen = sizeof(*pds);
		} else {
			datalen = sizeof(*pds) - sizeof(struct iec62439_portDS);
		}
		break;
	case MID_LOG_ANNOUNCE_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logAnnounceInterval;
		datalen = sizeof(*mtd);
		break;
	case MID_ANNOUNCE_RECEIPT_TIMEOUT:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->announceReceiptTimeout;
		datalen = sizeof(*mtd);
		break;
	case MID_LOG_SYNC_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logSyncInterval;
		datalen = sizeof(*mtd);
		break;
	case MID_VERSION_NUMBER:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->versionNumber;
		datalen = sizeof(*mtd);
		break;
	case MID_MASTER_ONLY:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->master_only;
		datalen = sizeof(*mtd);
		break;
	case MID_TRANSPARENT_CLOCK_PORT_DATA_SET:
		tcpds = (struct transparentClockPortDS *) tlv->data;
		tcpds->portIdentity            = rp->portIdentity;
		tcpds->faultyFlag              = (rp->state == PS_FAULTY);
		tcpds->logMinPdelayReqInterval = target->logMinPdelayReqInterval;
		tcpds->peerMeanPathDelay       = rp->peerMeanPathDelay;
		datalen = sizeof(*tcpds);
		if (clock_is_hsr_or_prp(target->clock)) {
			tcpds->iec62439_ds.portEnabled = 1;
			tcpds->iec62439_ds.dlyAsymmetry = 0;
			tcpds->iec62439_ds.twoStepFlag = 0; /* We don't support twostep for HSR/PRP for now */

			/* XXX: Not sure if other devices uses the
			 * same definition for their clockId (based on
			 * MAC), but ptp4l hides the transport layer from
			 * the PTP layer so this is the best we can do
			 * for now.
			 */
			memcpy(tcpds->iec62439_ds.peerIdentity, target->peer_portid.clockIdentity.id, 3);
			memcpy(&tcpds->iec62439_ds.peerIdentity[3], &target->peer_portid.clockIdentity.id[5], 3);

			tcpds->iec62439_ds.prpPairedPort = red_other_port(rp)->portIdentity.portNumber;
			if (clock_type(target->clock) == CLOCK_TYPE_BOUNDARY)
				tcpds->iec62439_ds.prpAttachment = PORT_TYPE_DABC;
			else
				tcpds->iec62439_ds.prpAttachment = PORT_TYPE_DATC;
			tcpds->iec62439_ds.errorCounter = target->errorCounter;
			tcpds->iec62439_ds.peerDelayLim = 100; /* Defined in IEC 62439-3 */
			datalen = sizeof(*tcpds);
		} else {
			datalen = sizeof(*tcpds) - sizeof(struct iec62439_transparent_portDS);
		}
		break;
	case MID_DELAY_MECHANISM:
		mtd = (struct management_tlv_datum *) tlv->data;
		if (target->delayMechanism)
			mtd->val = target->delayMechanism;
		else
			mtd->val = DM_E2E;
		datalen = sizeof(*mtd);
		break;
	case MID_LOG_MIN_PDELAY_REQ_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logMinPdelayReqInterval;
		datalen = sizeof(*mtd);
		break;
	case MID_PORT_DATA_SET_NP:
		pdsnp = (struct port_ds_np *) tlv->data;
		pdsnp->neighborPropDelayThresh = target->neighborPropDelayThresh;
		pdsnp->asCapable = target->asCapable;
		datalen = sizeof(*pdsnp);
		break;
	case MID_PORT_PROPERTIES_NP:
		ppn = (struct port_properties_np *)tlv->data;
		ppn->portIdentity = rp->portIdentity;
		if (target->state == PS_GRAND_MASTER)
			ppn->port_state = PS_MASTER;
		else
			ppn->port_state = rp->state;
		ppn->timestamping = target->timestamping;
		ts_label = interface_label(rp->iface);
		ptp_text_set(&ppn->interface, ts_label);
		datalen = sizeof(*ppn) + ppn->interface.length;
		break;
	case MID_PORT_STATS_NP:
		psn = (struct port_stats_np *)tlv->data;
		psn->portIdentity = target->portIdentity;
		psn->stats = target->stats;
		datalen = sizeof(*psn);
		break;
	case MID_PORT_SERVICE_STATS_NP:
		pssn = (struct port_service_stats_np *)tlv->data;
		pssn->portIdentity = target->portIdentity;
		pssn->stats = target->service_stats;
		datalen = sizeof(*pssn);
		break;
	case MID_PORT_HWCLOCK_NP:
		phn = (struct port_hwclock_np *)tlv->data;
		phn->portIdentity = rp->portIdentity;
		phn->phc_index = rp->phc_index;
		phn->flags = interface_get_vclock(rp->iface) >= 0 ?
			PORT_HWCLOCK_VCLOCK : 0;
		datalen = sizeof(*phn);
		break;
	case MID_POWER_PROFILE_SETTINGS_NP:
		pwr = (struct ieee_c37_238_settings_np *)tlv->data;
		memcpy(pwr, &target->pwr, sizeof(*pwr));
		datalen = sizeof(*pwr);
		break;
	default:
		/* The caller should *not* respond to this message. */
		tlv_extra_recycle(extra);
		return 0;
	}

	if (datalen % 2) {
		tlv->data[datalen] = 0;
		datalen++;
	}
	tlv->length = sizeof(tlv->id) + datalen;
	rsp->header.messageLength += sizeof(*tlv) + datalen;
	msg_tlv_attach(rsp, extra);

	/* The caller can respond to this message. */
	return 1;
}

static int red_port_management_get_response(struct red_port *rp,
					    struct port *ingress, int id,
					    struct ptp_message *req)

{
	struct PortIdentity pid = rp->portIdentity;
	struct ptp_message *rsp;
	int respond;

	rsp = port_management_reply(pid, ingress, req);
	if (!rsp) {
		return 0;
	}

	respond = red_port_management_fill_response(rp, rsp, id);
	if (respond) {
		if (port_is_red(ingress)) {
			red_prepare_and_send(ingress, rsp, TRANS_GENERAL);
		} else {
			port_prepare_and_send(ingress, rsp, TRANS_GENERAL);
		}
	}
	msg_put(rsp);
	return respond;
}

int red_management_get_response(struct port *target,
				struct port *ingress, int id,
				struct ptp_message *req)
{
	UInteger16 target_port = req->management.targetPortIdentity.portNumber;
	int respond = 0;

	if (target_port == target->red_a->portIdentity.portNumber) {
		respond += red_port_management_get_response(target->red_a, ingress, id, req);
	} else if (target_port == target->red_b->portIdentity.portNumber) {
		respond += red_port_management_get_response(target->red_b, ingress, id, req);
	} else {
		respond += red_port_management_get_response(target->red_a, ingress, id, req);
		respond += red_port_management_get_response(target->red_b, ingress, id, req);
	}

	return respond;
}

static void red_port_notify_event(struct red_port *rp, enum notification event)
{
	struct PortIdentity pid = rp->portIdentity;
	struct ptp_message *msg;
	int id;

	switch (event) {
	case NOTIFY_PORT_STATE:
		id = MID_PORT_DATA_SET;
		break;
	default:
		return;
	}
	/* targetPortIdentity and sequenceId will be filled by
	 * clock_send_notification */
	msg = port_management_notify(pid, rp->upper);
	if (!msg)
		return;
	if (!red_port_management_fill_response(rp, msg, id))
		goto err;
	if (msg_pre_send(msg))
		goto err;
	clock_send_notification(rp->clock, msg, event);
err:
	msg_put(msg);
}

bool red_portnum_is_red(struct port *p, int target)
{
	bool is_a = p->red_a->portIdentity.portNumber == target;
	bool is_b = p->red_b->portIdentity.portNumber == target;

	return is_a || is_b;
}

/* For HSR BC since passive/receiving BC needs to forward in HW like a TC */
static void red_port_set_socket_clk_type(struct red_port *rp, int clk_type)
{
	int event_fd = red_event_fd(rp);
	int header_offset;

	if (rp->upper->fda.fd[event_fd] < 0)
		return;

	header_offset = config_get_int(rp->trp->cfg, interface_label(rp->iface), "ptp_header_offset");
	sk_timestamping_init(rp->upper->fda.fd[event_fd], interface_label(rp->iface), clk_type,
			     rp->upper->timestamping, TRANS_IEEE_802_3, interface_get_vclock(rp->iface),
			     clock_domain_number(rp->clock), rp->upper->delayMechanism, header_offset);
}

/* HSR ports need to be put in TC mode when in passive/receiving state to forward in HW */
static void red_hsr_swap_clock_mode(struct port *p)
{
	enum hwtstamp_clk_types clktype;

	if (clock_type(p->clock) != CLOCK_TYPE_BOUNDARY || !clock_is_hsr(p->clock))
		return;

	if (p->state == PS_MASTER || p->state == PS_GRAND_MASTER) {
		if (p->curr_clktype == HWTSTAMP_CLOCK_TYPE_BOUNDARY_CLOCK)
			return;
		pr_info("Reconfiguring %s for BC", port_log_name(p));
		clktype = HWTSTAMP_CLOCK_TYPE_BOUNDARY_CLOCK;
	} else {
		if (p->curr_clktype == HWTSTAMP_CLOCK_TYPE_TRANSPARENT_CLOCK)
			return;
		pr_info("Reconfiguring %s for TC", port_log_name(p));
		clktype = HWTSTAMP_CLOCK_TYPE_TRANSPARENT_CLOCK;
	}
	red_port_set_socket_clk_type(p->red_a, clktype);
	red_port_set_socket_clk_type(p->red_b, clktype);
	p->curr_clktype = clktype;
}
