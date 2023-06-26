/**
 * @file ts2phc_pps_sink.c
 * @brief Utility program to synchronize the PHC clock to external events
 * @note Copyright (C) 2019 Balint Ferencz <fernya@sch.bme.hu>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <errno.h>
#include <linux/ptp_clock.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <gpiod.h>

#include "clockadj.h"
#include "config.h"
#include "missing.h"
#include "phc.h"
#include "print.h"
#include "servo.h"
#include "ts2phc.h"
#include "util.h"

struct ts2phc_pps_sink {
	char *name;
	STAILQ_ENTRY(ts2phc_pps_sink) list;
	struct ptp_pin_desc pin_desc;
	unsigned int polarity;
	tmv_t correction;
	uint32_t ignore_lower;
	uint32_t ignore_upper;
	struct ts2phc_clock *clock;
};

struct ts2phc_sink_array {
	struct ts2phc_pps_sink **sink;
	int *collected_events;
	struct pollfd *pfd;
};

enum extts_result {
	EXTTS_ERROR	= -1,
	EXTTS_OK	= 0,
	EXTTS_IGNORE	= 1,
};

static int ts2phc_pps_sink_array_create(struct ts2phc_private *priv)
{
	struct ts2phc_sink_array *polling_array;
	struct ts2phc_pps_sink *sink;
	unsigned int i;

	polling_array = malloc(sizeof(*polling_array));
	if (!polling_array)
		goto err_alloc_array;

	polling_array->sink = malloc(priv->n_sinks *
				     sizeof(*polling_array->sink));
	if (!polling_array->sink)
		goto err_alloc_sinks;

	polling_array->pfd = malloc(priv->n_sinks *
				    sizeof(*polling_array->pfd));
	if (!polling_array->pfd)
		goto err_alloc_pfd;

	polling_array->collected_events = malloc(priv->n_sinks * sizeof(int));
	if (!polling_array->collected_events)
		goto err_alloc_events;

	i = 0;
	STAILQ_FOREACH(sink, &priv->sinks, list) {
		polling_array->sink[i] = sink;
		i++;
	}
	for (i = 0; i < priv->n_sinks; i++) {
		struct ts2phc_pps_sink *sink = polling_array->sink[i];

		polling_array->pfd[i].events = POLLIN | POLLPRI;
		polling_array->pfd[i].fd = sink->clock->fd;
	}

	priv->polling_array = polling_array;

	return 0;

err_alloc_events:
	free(polling_array->pfd);
err_alloc_pfd:
	free(polling_array->sink);
err_alloc_sinks:
	free(polling_array);
err_alloc_array:
	pr_err("low memory");
	return -1;
}

static void ts2phc_pps_sink_array_destroy(struct ts2phc_private *priv)
{
	struct ts2phc_sink_array *polling_array = priv->polling_array;

	/* Allow sloppy calls of ts2phc_cleanup(), without having previously
	 * called ts2phc_pps_sink_array_create().
	 */
	if (!priv->polling_array)
		return;

	free(polling_array->collected_events);
	free(polling_array->sink);
	free(polling_array->pfd);
	free(polling_array);
}

static int ts2phc_pps_sink_clear_fifo(struct ts2phc_pps_sink *sink)
{
	struct pollfd pfd = {
		.events = POLLIN | POLLPRI,
		.fd = sink->clock->fd,
	};
	struct ptp_extts_event event;
	int cnt, size;

	while (1) {
		cnt = poll(&pfd, 1, 0);
		if (cnt < 0) {
			if (EINTR == errno) {
				continue;
			} else {
				pr_emerg("poll failed");
				return -1;
			}
		} else if (!cnt) {
			break;
		}
		size = read(pfd.fd, &event, sizeof(event));
		if (size != sizeof(event)) {
			pr_err("read failed");
			return -1;
		}
		pr_debug("%s SKIP extts index %u at %lld.%09u",
			 sink->name, event.index, event.t.sec, event.t.nsec);
	}

	return 0;
}

static struct ts2phc_pps_sink *ts2phc_pps_sink_create(struct ts2phc_private *priv,
						      const char *device)
{
	struct config *cfg = priv->cfg;
	struct ptp_extts_request extts;
	struct ts2phc_pps_sink *sink;
	int err, pulsewidth;
	int32_t correction;

	sink = calloc(1, sizeof(*sink));
	if (!sink) {
		pr_err("low memory");
		return NULL;
	}
	sink->name = strdup(device);
	if (!sink->name) {
		pr_err("low memory");
		free(sink);
		return NULL;
	}
	sink->pin_desc.index = config_get_int(cfg, device, "ts2phc.pin_index");
	sink->pin_desc.func = PTP_PF_EXTTS;
	sink->pin_desc.chan = config_get_int(cfg, device, "ts2phc.channel");
	sink->polarity = config_get_int(cfg, device, "ts2phc.extts_polarity");
	correction = config_get_int(cfg, device, "ts2phc.extts_correction");
	sink->correction = nanoseconds_to_tmv(correction);

	pulsewidth = config_get_int(cfg, device, "ts2phc.pulsewidth");
	pulsewidth /= 2;
	sink->ignore_upper = 1000000000 - pulsewidth;
	sink->ignore_lower = pulsewidth;

	sink->clock = ts2phc_clock_add(priv, device);
	if (!sink->clock) {
		pr_err("failed to open clock");
		goto no_posix_clock;
	}
	sink->clock->is_target = true;

	pr_debug("PPS sink %s has ptp index %d", device,
		 sink->clock->phc_index);

	if (phc_number_pins(sink->clock->clkid) > 0) {
		err = phc_pin_setfunc(sink->clock->clkid, &sink->pin_desc);
		if (err < 0) {
			pr_err("PTP_PIN_SETFUNC request failed");
			goto no_pin_func;
		}
	}

	/*
	 * Disable external time stamping, and then read out any stale
	 * time stamps.
	 */
	memset(&extts, 0, sizeof(extts));
	extts.index = sink->pin_desc.chan;
	extts.flags = 0;
	if (ioctl(sink->clock->fd, PTP_EXTTS_REQUEST2, &extts)) {
		pr_err(PTP_EXTTS_REQUEST_FAILED);
	}
	if (ts2phc_pps_sink_clear_fifo(sink)) {
		goto no_ext_ts;
	}

	return sink;
no_ext_ts:
no_pin_func:
	ts2phc_clock_destroy(sink->clock);
no_posix_clock:
	free(sink->name);
	free(sink);
	return NULL;
}

static void ts2phc_pps_sink_destroy(struct ts2phc_pps_sink *sink)
{
	struct ptp_extts_request extts;

	memset(&extts, 0, sizeof(extts));
	extts.index = sink->pin_desc.chan;
	extts.flags = 0;
	if (ioctl(sink->clock->fd, PTP_EXTTS_REQUEST2, &extts)) {
		pr_err(PTP_EXTTS_REQUEST_FAILED);
	}
	ts2phc_clock_destroy(sink->clock);
	free(sink->name);
	free(sink);
}

static bool ts2phc_pps_sink_ignore(struct ts2phc_private *priv,
				   struct ts2phc_pps_sink *sink,
				   struct timespec source_ts)
{
	tmv_t source_tmv = timespec_to_tmv(source_ts);

	source_tmv = tmv_sub(source_tmv, priv->perout_phase);
	source_ts = tmv_to_timespec(source_tmv);

	return source_ts.tv_nsec > sink->ignore_lower &&
	       source_ts.tv_nsec < sink->ignore_upper;
}

static enum extts_result ts2phc_pps_sink_event(struct ts2phc_private *priv,
					       struct ts2phc_pps_sink *sink)
{
	enum extts_result result = EXTTS_OK;
	struct ptp_extts_event event;
	struct timespec source_ts;
	int err, cnt;
	tmv_t ts;

	cnt = read(sink->clock->fd, &event, sizeof(event));
	if (cnt != sizeof(event)) {
		pr_err("read extts event failed: %m");
		result = EXTTS_ERROR;
		goto out;
	}
	if (event.index != sink->pin_desc.chan) {
		pr_err("extts on unexpected channel");
		result = EXTTS_ERROR;
		goto out;
	}

/*printf("Event time %s: %llu.%u\n", sink->name, event.t.sec, event.t.nsec);*/
	if (priv->use_gpio)
		goto out;

	err = ts2phc_pps_source_getppstime(priv->src, &source_ts);
	if (err < 0) {
		pr_debug("source ts not valid");
		return 0;
	}

	if (sink->polarity == (PTP_RISING_EDGE | PTP_FALLING_EDGE) &&
	    ts2phc_pps_sink_ignore(priv, sink, source_ts)) {

		pr_debug("%s SKIP extts index %u at %lld.%09u src %" PRIi64 ".%ld",
		 sink->name, event.index, event.t.sec, event.t.nsec,
		 (int64_t) source_ts.tv_sec, source_ts.tv_nsec);

		result = EXTTS_IGNORE;
		goto out;
	}

out:
	if (result == EXTTS_ERROR || result == EXTTS_IGNORE)
		return result;

	ts = pct_to_tmv(event.t);
	ts = tmv_add(ts, sink->correction);
	ts2phc_clock_add_tstamp(sink->clock, ts);

	return EXTTS_OK;
}

/* public methods */

int ts2phc_pps_sink_add(struct ts2phc_private *priv, const char *name)
{
	struct ts2phc_pps_sink *sink;

	/* Create each interface only once. */
	STAILQ_FOREACH(sink, &priv->sinks, list) {
		if (0 == strcmp(name, sink->name)) {
			return 0;
		}
	}
	sink = ts2phc_pps_sink_create(priv, name);
	if (!sink) {
		pr_err("failed to create sink");
		return -1;
	}
	STAILQ_INSERT_TAIL(&priv->sinks, sink, list);
	priv->n_sinks++;

	return 0;
}

int ts2phc_pps_sink_arm(struct ts2phc_private *priv)
{
	struct ptp_extts_request extts;
	struct ts2phc_pps_sink *sink;
	int err;

	memset(&extts, 0, sizeof(extts));

	STAILQ_FOREACH(sink, &priv->sinks, list) {
		extts.index = sink->pin_desc.chan;
		extts.flags = sink->polarity | PTP_ENABLE_FEATURE;
		err = ioctl(sink->clock->fd, PTP_EXTTS_REQUEST2, &extts);
		if (err < 0) {
			pr_err(PTP_EXTTS_REQUEST_FAILED);
			return -1;
		}
	}
	return 0;
}

int ts2phc_pps_sinks_init(struct ts2phc_private *priv)
{
	int err;

	err = ts2phc_pps_sink_array_create(priv);
	if (err)
		return err;

	return ts2phc_pps_sink_arm(priv);
}

void ts2phc_pps_sink_cleanup(struct ts2phc_private *priv)
{
	struct ts2phc_pps_sink *sink;

	ts2phc_pps_sink_array_destroy(priv);

	while ((sink = STAILQ_FIRST(&priv->sinks))) {
		STAILQ_REMOVE_HEAD(&priv->sinks, list);
		ts2phc_pps_sink_destroy(sink);
		priv->n_sinks--;
	}
}

int ts2phc_pps_sink_poll(struct ts2phc_private *priv)
{
	struct ts2phc_sink_array *polling_array = priv->polling_array;
	bool all_sinks_have_events = false;
	bool ignore_any = false;
	unsigned int i;
	int cnt;

	for (i = 0; i < priv->n_sinks; i++)
		polling_array->collected_events[i] = 0;

	while (!all_sinks_have_events) {
		struct ts2phc_pps_sink *sink;

		cnt = poll(polling_array->pfd, priv->n_sinks, 2000);
		if (cnt < 0) {
			if (errno == EINTR) {
				return 0;
			} else {
				pr_emerg("poll failed");
				return -1;
			}
		} else if (!cnt) {
			pr_debug("poll returns zero, no events");
			return 0;
		}

		for (i = 0; i < priv->n_sinks; i++) {
			if (polling_array->pfd[i].revents & (POLLIN|POLLPRI)) {
				enum extts_result result;

				sink = polling_array->sink[i];

				result = ts2phc_pps_sink_event(priv, sink);
				if (result == EXTTS_ERROR)
					return -EIO;
				if (result == EXTTS_IGNORE)
					ignore_any = true;

				/*
				 * Collect the events anyway, even if we'll
				 * ignore this source edge later. We don't
				 * want sink events from different edges
				 * to pile up and mix.
				 */
				polling_array->collected_events[i]++;
			}
		}

		all_sinks_have_events = true;

		for (i = 0; i < priv->n_sinks; i++) {
			if (!polling_array->collected_events[i]) {
				all_sinks_have_events = false;
				break;
			}
		}
	}

	if (ignore_any)
		return 0;

	return 1;
}

static int ts2phc_pps_sink_get_polarity(struct ts2phc_private *priv)
{
	struct ts2phc_pps_sink *sink;

	/* Assume that we have at least one sink and all sinks have the same polarity */
	STAILQ_FOREACH(sink, &priv->sinks, list) {
		return sink->polarity;
	}

	return -1;
}

int ts2phc_gpio_trigger_pulse(struct ts2phc_private *priv)
{
	int polarity;
	int err = 0;

	polarity = priv->gpio_polarity;
	if (polarity == (PTP_RISING_EDGE | PTP_FALLING_EDGE)) {
		gpiod_line_set_value(priv->line, priv->last_edge_rising ? 0 : 1);
		priv->last_edge_rising = priv->last_edge_rising ? 0 : 1;
	} else if (polarity == PTP_RISING_EDGE) {
		gpiod_line_set_value(priv->line, 0);
		usleep(1000);
		gpiod_line_set_value(priv->line, 1);
		usleep(1000);
	} else if (polarity == PTP_FALLING_EDGE) {
		gpiod_line_set_value(priv->line, 1);
		gpiod_line_set_value(priv->line, 0);
	}
	return err;
}

int ts2phc_gpio_init_port(struct ts2phc_private *priv, struct config *cfg, const char *dev)
{
	struct ts2phc_pps_sink *sink;
	struct ts2phc_clock *clock;
	int err, found = 0;
	char *chipname;
	int ena_pin;

	STAILQ_FOREACH(sink, &priv->sinks, list) {
		if (0 == strcmp(dev, sink->name)) {
			found = 1;
			break;
		}
	}

	if (!found)
		return -ENODEV;

	clock = sink->clock;

	if (config_get_int(cfg, dev, "ts2phc.gpio_master"))
		priv->ref_clock = clock;

	chipname = config_get_string(cfg, dev, "ts2phc.gpio_enable_chip");
	ena_pin = config_get_int(cfg, dev, "ts2phc.gpio_enable_pin");

	if (chipname == NULL) {
		pr_warning("warning: no gpio_enable_chip provided. Ignore if HW does not need to enable gpio access");
		return 0;
	}

	if (ena_pin == -1) {
		pr_err("gpio_enable_chip used without corresponding gpio_enable_pin");
		return -EINVAL;
	}

	clock->chip = gpiod_chip_open_by_name(chipname);
	if (!clock->chip) {
		pr_err("could not open chipname %s\n", chipname);
		return -ENODEV;
	}

	clock->ena_line = gpiod_chip_get_line(clock->chip, ena_pin);
	if (!clock->ena_line) {
		pr_err("could not get gpio line %d on chip %s\n", ena_pin, chipname);
		return -ENODEV;
	}

	/* Default to 1 so it is enabled immediately */
	err = gpiod_line_request_output(clock->ena_line, "ts2phc", 1);
	if (err) {
		/* If multiple PTP clocks share the same enable pin this is
		 * expected. And the pin should have been enabled by the first
		 * clock that runs this function.
		 */
		pr_warning("warning: chip %s line %d could not be requested. May be shared between the clocks\n",
			   chipname, ena_pin);
	}
	return 0;
}

int ts2phc_gpio_request_out(struct ts2phc_private *priv, struct config *cfg)
{
	char *chipname;
	int default_value;
	int out_pin;
	int err;

	priv->gpio_polarity = ts2phc_pps_sink_get_polarity(priv);
	chipname = config_get_string(cfg, NULL, "ts2phc.gpio_chip");
	out_pin = config_get_int(cfg, NULL, "ts2phc.gpio_pin");

	if (chipname == NULL) {
		pr_err("warning: no gpio_chip provided");
		return -EINVAL;
	}

	if (out_pin == -1) {
		pr_err("gpio_chip used without corresponding gpio_pin");
		return -EINVAL;
	}

	priv->chip = gpiod_chip_open_by_name(chipname);
	if (!priv->chip) {
		pr_err("%s: could not open chipname %s\n", __func__, chipname);
		return -ENODEV;
	}

	priv->line = gpiod_chip_get_line(priv->chip, out_pin);
	if (!priv->line) {
		pr_err("%s: could not get gpio line %d on chip %s\n", __func__, out_pin, chipname);
		return -ENODEV;
	}

	/* If rising/both then default to 0. If falling default to 1. */
	default_value = priv->gpio_polarity & PTP_RISING_EDGE ? 0 : 1;
	err = gpiod_line_request_output(priv->line, "ts2phc", default_value);
	if (err) {
		pr_err("chip %s line %d could not be requested\n",
		       chipname, out_pin);
		return -ENODEV;
	}
	return 0;
}

void ts2phc_gpio_release(struct ts2phc_private *priv)
{
	struct ts2phc_pps_sink *sink;

	if (priv->line) {
		gpiod_line_set_value(priv->line, 0);
		gpiod_line_release(priv->line);
	}

	STAILQ_FOREACH(sink, &priv->sinks, list) {
		if (sink->clock->ena_line) {
			gpiod_line_set_value(sink->clock->ena_line, 0);
			gpiod_line_release(sink->clock->ena_line);
		}
	}
}
