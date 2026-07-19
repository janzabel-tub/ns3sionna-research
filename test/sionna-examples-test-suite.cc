/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/example-as-test.h"

static ns3::ExampleAsTestSuite g_sionnaSuMimoMinimalExample(
    "sionna-example-sumimo-minimal",
    "ns3_wifi_sumimo_minimal",
    NS_TEST_SOURCEDIR,
    "--mcs=5 --referenceSnrDb=20 --packetSize=1200 --eesmBetaLinear=2");
