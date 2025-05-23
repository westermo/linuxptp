/**
 * @file ptpmon.c
 * @brief Utility program to gather sync information from multiple ptp4l instances.
 * @note Copyright (C) 2025 Casper Andersson <casper.casan@gmail.com>
 * @note SPDX-License-Identifier: GPL-2.0+
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>

/* #include "config.h" */
#include "config.h"
#include "print.h"
#include "tlv.h"
#include "util.h"
#include "version.h"

#include "pmc_agent.h"

#define MAX_AGENTS 30

/*
  
--relative-to /var/run/ptp4l-eth3  # Listen to socket and use as reference
-i /var/run/ptp4l                  # Listen to socket
-i /var/run/ptp4l-eth4             # Listen to socket
-t/--target 198.18.100.1           # Send information to remote destination
-d/--domain 254                    # ptp4l domain
-n/--name                          # Instance name

--only-sync                        # Only send sync status
--only-states                      # Only send port transitions

-m
-l
-v
-h
-q

# config file

name dut-a

[/var/run/ptp4l-eth3]
name eth3


##########


Send and log as "dut-a:eth3"

If an instances has multiple ports (when reporting state transitions),
only report the PortIdentity. Skip converting to actual name as we
would have to query PORT_DATA_SET_NP.

*/







#define MAX_PATH_LEN 128

struct ptpmon_agent {
	LIST_ENTRY(ptpmon_agent) list;
	struct ptpmon *ptpmon;
	struct pmc_agent *agent;
	/* Make a copy of config for each agent since it uses the "uds_address" field */
	struct config *cfg;
	int is_master;
	char *name;
	char path[MAX_PATH_LEN];
};

struct ptpmon {
	/* tmv_t relative_last_offset; */
	struct config *cfg;
	struct sockaddr_in server;
	uint16_t tcp_port;
	int target_socket;
	/* char *name; */
	LIST_HEAD(agents_head, ptpmon_agent) agents;
};

static void ptpmon_cleanup(struct ptpmon *priv)
{
	struct ptpmon_agent *pm_agent;

	if (priv->cfg)
		config_destroy(priv->cfg);

	LIST_FOREACH(pm_agent, &priv->agents, list) {
		printf("Destroying\n");
		pmc_agent_destroy(pm_agent->agent);
		config_destroy(pm_agent->cfg);
	}

	/* if (priv->agent) */
		/* pmc_agent_destroy(priv->agent); */
}

static int ptpmon_recv_subscribed(void *context, struct ptp_message *msg,
				  int excluded)
{
	struct ptpmon_agent *agent = context;
	/* struct portDS *pds; */
	struct time_status_np *tsn;
	int64_t sec, nsec;
	int mgt_id;
	

	mgt_id = management_tlv_id(msg);
	if (mgt_id == excluded)
		return 0;

	pr_info("Name: %s", agent->name);

	switch (mgt_id) {
	case MID_PORT_DATA_SET:
		/* pds = management_tlv_data(msg); */
		pr_info("Received PORT_DS");
		/* port = ts2phc_port_get(priv, pds->portIdentity.portNumber); */
		/* if (!port) { */
		/* 	pr_info("received data for unknown port %s", */
		/* 		pid2str(&pds->portIdentity)); */
		/* 	return 1; */
		/* } */
		/* state = port_state_normalize(pds->portState); */
		/* if (port->state != state) { */
		/* 	pr_info("port %s changed state", */
		/* 		pid2str(&pds->portIdentity)); */
		/* 	port->state = state; */
		/* 	clock = port->clock; */
		/* 	state = ts2phc_clock_compute_state(priv, clock); */
		/* 	if (clock->state != state || clock->new_state) { */
		/* 		clock->new_state = state; */ /* 		priv->state_changed = true; */
		/* 	} */
		/* } */
		return 1;
	case MID_TIME_STATUS_NP:
		tsn = management_tlv_data(msg);
		sec = tsn->ingress_time / NSEC_PER_SEC;
		nsec = tsn->ingress_time % NSEC_PER_SEC;
		pr_info("GmIdentity %s. IngressTime %"PRId64".%09"PRId64". SyncSeq %5"PRIu16". Offset %10"PRId64". PathDelay %10"PRId64"",
		       cid2str(&tsn->gmIdentity), sec, nsec, tsn->last_sync_seqid, tsn->master_offset, tsn->mean_path_delay);
		char *ptr;
		int bytes;
		// +++ adds a new line because otherwise server fails to parse the name
		// TODO: replace with fixed-size array
		bytes = asprintf(&ptr, "+++++\nname %s\ngm_identity %s\ningr_time %"PRId64"\nlast_sync_seq %"PRIu16"\noffset %"PRId64"\npath_delay %"PRId64"\n\n",
		       agent->name, cid2str(&tsn->gmIdentity), tsn->ingress_time, tsn->last_sync_seqid, tsn->master_offset, tsn->mean_path_delay);
		if (ptr) {
			write(agent->ptpmon->target_socket, ptr, bytes);
			free(ptr);
		}

		return 1;
	case MID_PARENT_DATA_SET:
		pr_info("Received PARENT_DS");
		return 1;
	}
	return 0;
}

/* static struct config *copy_config(struct config *origin, char *config) */
/* { */
/* 	struct config *cfg; */
/* 	int c; */

/* 	cfg = config_create(); */
/* 	if (!cfg) { */
/* 		fprintf(stderr, "failed to create config\n"); */
/* 		return NULL; */
/* 	} */

/* 	if (config && (c = config_read(config, cfg))) { */
/* 		fprintf(stderr, "failed to read config\n"); */
/* 		return NULL; */
/* 	} */

/* 	return cfg; */
/* } */

int main(int argc, char *argv[])
{
	char uds_local[MAX_IFNAME_SIZE + 1];
	char *config = NULL, *progname;
	struct ptpmon_agent *pm_agent;
	struct config *cfg = NULL;
	/* struct pmc_agent *agent; */
	struct option *opts;
	struct ptpmon priv;
	/* char *paths_list[MAX_AGENTS]; */
	/* int paths_count = 0; */
	char *names_list[MAX_AGENTS];
	int names_count = 0;
	char *ip = NULL;
	int err, index;
	int c;

	handle_term_signals();

	cfg = config_create();
	if (!cfg) {
		ptpmon_cleanup(&priv);
		return -1;
	}

	priv.cfg = cfg;

	opts = config_long_options(cfg);

	progname = strrchr(argv[0], '/');
	progname = progname ? 1 + progname : argv[0];
	while (EOF != (c = getopt_long(argc, argv, "d:i:f:hl:mqs:vn:t:", opts, &index))) {
		switch (c) {
		case 0:
			if (config_parse_option(cfg, opts[index].name, optarg)) {
				ptpmon_cleanup(&priv);
				return -1;
			}
			break;
		case 'f':
			config = optarg;
			break;
		case 'l':
			if (get_arg_val_i(c, optarg, &print_level,
					  PRINT_LEVEL_MIN, PRINT_LEVEL_MAX)) {
				ptpmon_cleanup(&priv);
				return -1;
			}
			config_set_int(cfg, "logging_level", print_level);
			print_set_level(print_level);
			break;
		case 'm':
			config_set_int(cfg, "verbose", 1);
			print_set_verbose(1);
			break;
		case 'n':
			if (names_count >= MAX_AGENTS) {
				pr_err("Max agents reached. Couldn't add %s", optarg);
			}
			names_list[names_count] = optarg;
			names_count++;
			break;
		/* case 'q': */
		/* 	config_set_int(cfg, "use_syslog", 0); */
		/* 	print_set_syslog(0); */
		/* 	break; */
		case 'd':
			if (config_set_int(cfg, "domainNumber", atoi(optarg))) {
				return -1;
			}
			break;
		/* case 'i': */
		/* 	if (paths_count >= MAX_AGENTS) { */
		/* 		pr_err("Max agents reached. Couldn't add %s", optarg); */
		/* 	} */
		/* 	paths_list[paths_count] = optarg; */
		/* 	paths_count++; */
		/* 	break; */
		case 't':
			ip = optarg;
			break;
		case 'v':
			ptpmon_cleanup(&priv);
			version_show(stdout);
			return 0;
		case 'h':
			ptpmon_cleanup(&priv);
			/* usage(progname); */
			return -1;
		case '?':
		default:
			ptpmon_cleanup(&priv);
			/* usage(progname); */
			return -1;
		}
	}
	
	print_set_progname(progname);

	if (config && (c = config_read(config, cfg))) {
		fprintf(stderr, "failed to read config\n");
		ptpmon_cleanup(&priv);
		return -1;
	}

	print_set_tag(config_get_string(cfg, NULL, "message_tag"));
	print_set_verbose(config_get_int(cfg, NULL, "verbose"));
	print_set_syslog(config_get_int(cfg, NULL, "use_syslog"));
	print_set_level(config_get_int(cfg, NULL, "logging_level"));
	
	LIST_INIT(&priv.agents);
	
	priv.target_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (priv.target_socket == -1) {
		printf("socket creation failed...\n");
		goto out;
	} else {
		printf("Socket successfully created..\n");
	}
	
	if (ip == NULL)
		ip = "127.0.0.1";
	// TODO: make configurable
	priv.tcp_port = 9876;
	priv.server.sin_family = AF_INET;
	priv.server.sin_port = htons(priv.tcp_port);
	priv.server.sin_addr.s_addr = inet_addr(ip);

	if (connect(priv.target_socket, &priv.server, sizeof(struct sockaddr_in)) != 0) {
		printf("connection with the server failed...\n");
		goto out;
	} else {
		printf("connected to the server..\n");
	}
	
	/* TODO: handle files properly */
	for (int i = 0; i < names_count; i++) {
		pm_agent = calloc(1, sizeof(struct ptpmon_agent));
		if (!pm_agent) {
			pr_err("Failed allocation");
			return -1;
		}
		pm_agent->name = names_list[i];
		pm_agent->agent = pmc_agent_create();
		pm_agent->ptpmon = &priv;
		pm_agent->cfg = config_create();
		snprintf(pm_agent->path, MAX_PATH_LEN, "/var/run/ptp-%s", pm_agent->name);
		pr_notice("%s", pm_agent->path);
		config_set_string(pm_agent->cfg, "uds_address", pm_agent->path);
		config_set_int(pm_agent->cfg, "domainNumber", config_get_int(cfg, NULL, "domainNumber"));
		config_set_int(pm_agent->cfg, "transportSpecific", config_get_int(cfg, NULL, "transportSpecific"));
		config_set_int(pm_agent->cfg, "logging_level", config_get_int(cfg, NULL, "logging_level"));
		/* printf("%s\n", paths_list[i]); */
		// TODO: Add proper names through config file
		/* if (i < names_count) */
		/* else */
			/* pm_agent->name = paths_list[i]; */

		snprintf(uds_local, sizeof(uds_local), "/var/run/ptpmon.%d", i);
		err = init_pmc_node(cfg, pm_agent->agent, uds_local,
				    ptpmon_recv_subscribed, pm_agent);
		if (err) {
			pr_err("Failed creating PMC node");
			return -1;
		}
		err = pmc_agent_subscribe_all(pm_agent->agent, 1000, 1);
		if (err) {
			pr_err("failed to subscribe");
			return -1;
		}
		LIST_INSERT_HEAD(&priv.agents, pm_agent, list);
	}

	while (is_running()) {
		LIST_FOREACH(pm_agent, &priv.agents, list) {
			err = pmc_agent_update_subscribe_all(pm_agent->agent);
			if (err < 0) {
				pr_err("pmc_agent_update returned %d", err);
				goto out;
			}
		}
		/* printf("Sleeping...\n"); */
		usleep(1000000);
	}
	

out:
	if (priv.target_socket >= 0)
		close(priv.target_socket);
	/* Cleanup subscriptions */
	LIST_FOREACH(pm_agent, &priv.agents, list) {
		send_unsubscribe_all(pm_agent->agent);
	}

	/* priv.agent = pmc_agent_create(); */
	/* if (!priv.agent) { */
		/* ptpmon_cleanup(&priv); */
		/* return -1; */
	/* } */

	/* err = init_pmc_node(cfg, priv.agent, uds_local, */
			    /* ts2phc_recv_subscribed, &priv); */
	ptpmon_cleanup(&priv);
}






