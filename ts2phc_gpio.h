/**
 * @file ts2phc_gpio.h
 * @brief Utility program to synchronize the PHC clock to external events
 * @note Copyright (C) 2019 Balint Ferencz <fernya@sch.bme.hu>
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#ifndef HAVE_TS2PHC_GPIO_H
#define HAVE_TS2PHC_GPIO_H

#include "ts2phc.h"

int ts2phc_pps_sink_get_polarity(struct ts2phc_private *priv);

#endif

