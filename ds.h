/**
 * @file ds.h
 * @brief Data sets
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
#ifndef HAVE_DS_H
#define HAVE_DS_H

#include "ddt.h"
#include "fault.h"
#include "filter.h"
#include "tsproc.h"

/* clock data sets */

#define DDS_TWO_STEP_FLAG (1<<0)
#define DDS_SLAVE_ONLY    (1<<1)

#define PROFILE_SET_L3E2E     (1<<7)
#define PROFILE_SET_L2P2P     (1<<8)
#define PROFILE_SET_61850_9_3 (1<<9)

/* IEC 62439-3 */
enum portAttachmentType {
        PORT_TYPE_NOT_SPECIFIED,
        PORT_TYPE_OC,
        PORT_TYPE_BC,
        PORT_TYPE_TC,
        PORT_TYPE_BOUNDARY_NODE,
        PORT_TYPE_DAN_OC,
        PORT_TYPE_DABC,
        PORT_TYPE_DATC,
        PORT_TYPE_SLTC,
        PORT_TYPE_UNKNOWN = 255,
};

/* Based on IEC 62439-3 2016 */
struct iec62439_defaultDS {
	UInteger32           profileSet;
	TimeInterval         timeInaccuracy;
	UInteger32           offsetFromMasterLim;
} PACKED;

struct iec62439_transparent_defaultDS {
	UInteger32           profileSet;
	TimeInterval         timeInaccuracy;
} PACKED;

struct iec62439_portDS {
	Boolean             portEnabled;
	TimeInterval        dlyAsymmetry;
	UInteger32          profileId; /* Same as iec62439_defaultDS.profileSet */
	Boolean             vlanEnable;
	UInteger32          vlanId;
	UInteger32          vlanPrio;
	Boolean             twoStepFlag;
	Octet               peerIdentity[6];
	enum portAttachmentType prpAttachment;
	UInteger16          prpPairedPort;
	UInteger32          errorCounter;
	TimeInterval        peerDelayLim;
} PACKED;

struct iec62439_transparent_portDS {
	Boolean             portEnabled;
	TimeInterval        dlyAsymmetry;
	Boolean             twoStepFlag;
	Octet               peerIdentity[6];
	enum portAttachmentType prpAttachment;
	UInteger16          prpPairedPort;
	UInteger32          errorCounter;
	TimeInterval        peerDelayLim;
} PACKED;

struct defaultDS {
	UInteger8            flags;
	UInteger8            reserved1;
	UInteger16           numberPorts;
	UInteger8            priority1;
	struct ClockQuality  clockQuality;
	UInteger8            priority2;
	struct ClockIdentity clockIdentity;
	UInteger8            domainNumber;
	UInteger8            reserved2;
	struct iec62439_defaultDS iec62439_ds;
} PACKED;

#define OUI_LEN 3
struct clock_description {
	struct static_ptp_text productDescription;
	struct static_ptp_text revisionData;
	struct static_ptp_text userDescription;
	Octet manufacturerIdentity[OUI_LEN];
};

struct dataset {
	UInteger8            priority1;
	struct ClockIdentity identity;
	struct ClockQuality  quality;
	UInteger8            priority2;
	UInteger8            localPriority; /* Telecom Profile only */
	UInteger16           stepsRemoved;
	struct PortIdentity  sender;
	struct PortIdentity  receiver;
};

struct currentDS {
	UInteger16   stepsRemoved;
	TimeInterval offsetFromMaster;
	TimeInterval meanPathDelay;
} PACKED;

struct parentDS {
	struct PortIdentity  parentPortIdentity;
	UInteger8            parentStats;
	UInteger8            reserved;
	UInteger16           observedParentOffsetScaledLogVariance;
	Integer32            observedParentClockPhaseChangeRate;
	UInteger8            grandmasterPriority1;
	struct ClockQuality  grandmasterClockQuality;
	UInteger8            grandmasterPriority2;
	struct ClockIdentity grandmasterIdentity;
} PACKED;

struct parent_ds {
	struct parentDS pds;
	struct ClockIdentity *ptl;
	unsigned int path_length;
};

#define CURRENT_UTC_OFFSET  37 /* 1 Jan 2017 */
#define INTERNAL_OSCILLATOR 0xA0
#define CLOCK_CLASS_THRESHOLD_DEFAULT 248

struct timePropertiesDS {
	Integer16    currentUtcOffset;
	UInteger8    flags;
	Enumeration8 timeSource;
} PACKED;

struct portDS {
	struct PortIdentity portIdentity;
	Enumeration8        portState;
	Integer8            logMinDelayReqInterval;
	TimeInterval        peerMeanPathDelay;
	Integer8            logAnnounceInterval;
	UInteger8           announceReceiptTimeout;
	Integer8            logSyncInterval;
	Enumeration8        delayMechanism;
	Integer8            logMinPdelayReqInterval;
	UInteger8           versionNumber;
	struct iec62439_portDS iec62439_ds;
} PACKED;

struct transparentClockDefaultDS {
	struct ClockIdentity clockIdentity;
	UInteger16           numberPorts;
	Enumeration8         delayMechanism;
	UInteger8            primaryDomain;
	struct iec62439_transparent_defaultDS iec62439_ds;
} PACKED;

struct transparentClockPortDS {
	struct PortIdentity portIdentity;
	Integer8            faultyFlag;
	Integer8            logMinPdelayReqInterval;
	TimeInterval        peerMeanPathDelay;
	struct iec62439_transparent_portDS iec62439_ds;
} PACKED;

#define FRI_ASAP (-128)

#endif
