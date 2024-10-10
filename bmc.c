/**
 * @file bmc.c
 * @note Copyright (C) 2011 Richard Cochran <richardcochran@gmail.com>
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
#include <string.h>

#include "bmc.h"
#include "clock.h"
#include "ds.h"
#include "port.h"
#include "print.h"

static int portid_cmp(struct PortIdentity *a, struct PortIdentity *b)
{
	int diff = memcmp(&a->clockIdentity, &b->clockIdentity, sizeof(a->clockIdentity));

	if (diff == 0) {
		diff = a->portNumber - b->portNumber;
	}

	return diff;
}

int dscmp2(struct dataset *a, struct dataset *b)
{
	int diff;
	unsigned int A = a->stepsRemoved, B = b->stepsRemoved;

	if (A + 1 < B)
		return A_BETTER;
	if (B + 1 < A)
		return B_BETTER;
	/*
	 * We ignore the "error-1" conditions mentioned in the
	 * standard, since there is nothing we can do about it anyway.
	 */
	if (A < B) {
		diff = portid_cmp(&b->receiver, &b->sender);
		if (diff < 0)
			return A_BETTER;
		if (diff > 0)
			return A_BETTER_TOPO;
		/* error-1 */
		return 0;
	}
	if (A > B) {
		diff = portid_cmp(&a->receiver, &a->sender);
		if (diff < 0)
			return B_BETTER;
		if (diff > 0)
			return B_BETTER_TOPO;
		/* error-1 */
		return 0;
	}

	diff = portid_cmp(&a->sender, &b->sender);
	if (diff < 0)
		return A_BETTER_TOPO;
	if (diff > 0)
		return B_BETTER_TOPO;

	if (a->receiver.portNumber < b->receiver.portNumber)
		return A_BETTER_TOPO;
	if (a->receiver.portNumber > b->receiver.portNumber)
		return B_BETTER_TOPO;
	/*
	 * If we got this far, it means "error-2" has occured.
	 */
	return 0;
}

int dscmp(struct dataset *a, struct dataset *b)
{
	int diff;

	if (a == b)
		return 0;
	if (a && !b)
		return A_BETTER;
	if (b && !a)
		return B_BETTER;

	diff = memcmp(&a->identity, &b->identity, sizeof(a->identity));

	if (!diff)
		return dscmp2(a, b);

	if (a->priority1 < b->priority1)
		return A_BETTER;
	if (a->priority1 > b->priority1)
		return B_BETTER;

	if (a->quality.clockClass < b->quality.clockClass)
		return A_BETTER;
	if (a->quality.clockClass > b->quality.clockClass)
		return B_BETTER;

	if (a->quality.clockAccuracy < b->quality.clockAccuracy)
		return A_BETTER;
	if (a->quality.clockAccuracy > b->quality.clockAccuracy)
		return B_BETTER;

	if (a->quality.offsetScaledLogVariance <
	    b->quality.offsetScaledLogVariance)
		return A_BETTER;
	if (a->quality.offsetScaledLogVariance >
	    b->quality.offsetScaledLogVariance)
		return B_BETTER;

	if (a->priority2 < b->priority2)
		return A_BETTER;
	if (a->priority2 > b->priority2)
		return B_BETTER;

	return diff < 0 ? A_BETTER : B_BETTER;
}

static char *compare_to_str(int res)
{
	switch (res) {
	case A_BETTER:
		return "A_BETTER";
	case B_BETTER:
		return "B_BETTER";
	case A_BETTER_TOPO:
		return "A_BETTER_TOPO";
	case B_BETTER_TOPO:
		return "B_BETTER_TOPO";
	default:
		return "EQUAL";
	}
}

/* static void print_compare(const char *a_str, const char *b_str, int res) */
/* { */
/* 	pr_err("A %s | B %s | Compare = %s\n", a_str, b_str, compare_to_str(res)); */
/* } */

/* static void print_ds(const char* str, const struct dataset *d) */
/* { */
/* 	pr_err("Dataset on %s\n", str); */
/* 	if (!d) { */
/* 		pr_err("Nothing to show yet\n"); */
/* 		return; */
/* 	} */
/* 	pr_err("Identity: %s\n", cid2str(&d->identity)); */
/* 	pr_err("Priority1: %d\n", d->priority1); */
/* 	pr_err("StepsRemoved: %d\n", d->stepsRemoved); */
/* 	pr_err("Sender: %s\n", pid2str(&d->sender)); */
/* 	pr_err("Receiver: %s\n", pid2str(&d->receiver)); */
/* } */

static enum port_state hsr_state_decision(struct clock *c, struct port *r,
					  int (*compare)(struct dataset *a, struct dataset *b))
{
	struct dataset *clock_best, *port_best, *pair_best;
	struct port *q;

	q = port_get_paired(r);
	clock_best = clock_best_foreign(c);
	port_best = port_best_foreign(r);
	pair_best = port_best_foreign(q);

	/* print_ds("Clock", clock_best); */
	/* print_ds(port_log_name(r), port_best); */
	/* print_ds(port_log_name(q), pair_best); */
	/* print_compare(port_log_name(r), port_log_name(q), compare(port_best, clock_best)); */
	/* print_compare("Clock", port_log_name(r), compare(clock_best, port_best)); */
	/* print_compare("Clock", port_log_name(q), compare(clock_best, pair_best)); */

	/* IEC62439-3: A.5.4. b) and c). SLAVE */
	if (compare(port_best, clock_best) == 0 || compare(pair_best, clock_best) == 0) {
		if (compare(port_best, pair_best) > 0) {
			/* Sticky: stay passive if other port is active */
			if (port_state(q) == PS_UNCALIBRATED) {
				pr_debug("State %s: PS_PASSIVE_SLAVE 1\n", port_log_name(r));
				return PS_PASSIVE_SLAVE;
			}
			pr_debug("State %s: PS_SLAVE 1\n", port_log_name(r));
			return PS_SLAVE;
		} else {
			/* Sticky: stay active if other port is
			 * passive. If we are comming directly from
			 * MASTER it's an indication of
			 * ANNOUNCE_RECEIPT_TIMEOUT on this port. Don't
			 * behave like sticky in this case.
			 */
			if (port_state(q) == PS_PASSIVE_SLAVE && port_state(r) != PS_MASTER) {
				pr_debug("State %s: PS_SLAVE 2\n", port_log_name(r));
				return PS_SLAVE;
			}
			pr_debug("State %s: PS_PASSIVE_SLAVE 2\n", port_log_name(r));
			return PS_PASSIVE_SLAVE;
		}
	}

	if (!port_best && !pair_best) {
		pr_debug("State %s: PS_MASTER 1\n", port_log_name(r));
		return PS_MASTER;
	}

	int res1 = compare(clock_best, port_best);
	int res2 = compare(clock_best, pair_best);

	/* The primary Master should have res1 and res2 as
	 * A_BETTER. Redundant Master should have one or both as
	 * A_BETTER_TOPO, in case of one link being broken that
	 * one will be A_BETTER.
	 *
	 * A_BETTER means better qualification.
	 * A_BETTER_TOPO means better topology (fewer stepsRemoved)
	 */
	if (res1 > 0 && res2 > 0) {
		/* /\* IEC62439-3: A.5.4. a) Active MASTER *\/ */
		if (res1 >= A_BETTER && res2 >= A_BETTER) {
			pr_debug("State %s: PS_MASTER 2\n", port_log_name(r));
			return PS_MASTER;
		}
		/* IEC62439-3: A.5.4. d) Redundant MASTER */
		if (clock_type(c) != CLOCK_TYPE_E2E && clock_type(c) != CLOCK_TYPE_P2P) {
			pr_debug("State %s: PS_PASSIVE 1\n", port_log_name(r));
			return PS_PASSIVE;
		}
	}
	/* IEC62439-3: A.5.4. d) Redundant MASTER */
	if (clock_type(c) != CLOCK_TYPE_E2E && clock_type(c) != CLOCK_TYPE_P2P) {
		if (compare(port_best, pair_best) != 0) {
			pr_debug("State %s: PS_PASSIVE 2\n", port_log_name(r));
			return PS_PASSIVE;
		}
	}

	pr_err("HSR BMC state decision failed %s", port_log_name(r));
	return PS_FAULTY; /* Not sure what to do here. Throw FAULTY for now. */
}

enum port_state bmc_state_decision(struct clock *c, struct port *r,
				   int (*compare)(struct dataset *a, struct dataset *b))
{
	struct dataset *clock_ds, *clock_best, *port_best;
	enum port_state ps;

	clock_ds = clock_default_ds(c);
	clock_best = clock_best_foreign(c);
	port_best = port_best_foreign(r);
	ps = port_state(r);

	/*
	 * This scenario is particularly important in the designated_slave_fsm
	 * when it is in PS_SLAVE state. In this scenario, there is no other
	 * foreign master and it will elect itself as master ultimately
	 * resulting in printing out some unnecessary warnings (see
	 * port_slave_priority_warning()).
	 */
	if (!port_best && port_bmca(r) == BMCA_NOOP) {
		return ps;
	}

	if (!port_best && PS_LISTENING == ps)
		return ps;

	if ((clock_is_hsr(c) || clock_is_prp(c)) && port_get_paired(r)) {
		return hsr_state_decision(c, r, compare);
	}

	if (clock_class(c) <= 127) {
		if (compare(clock_ds, port_best) > 0) {
			return PS_GRAND_MASTER; /*M1*/
		} else {
			return PS_PASSIVE; /*P1*/
		}
	}

	if (compare(clock_ds, clock_best) > 0) {
		return PS_GRAND_MASTER; /*M2*/
	}

	if (clock_best_port(c) == r) {
		return PS_SLAVE; /*S1*/
	}

	if (compare(clock_best, port_best) == A_BETTER_TOPO) {
		return PS_PASSIVE; /*P2*/
	} else {
		return PS_MASTER; /*M3*/
	}
}
