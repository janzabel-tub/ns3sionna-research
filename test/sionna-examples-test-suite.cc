/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#include "ns3/example-as-test.h"

#include <string>

class SionnaSuMimoMinimalTest : public ns3::ExampleAsTestCase
{
  public:
    SionnaSuMimoMinimalTest()
        : ExampleAsTestCase(
              "sionna-example-sumimo-minimal",
              "ns3_wifi_sumimo_minimal",
              NS_TEST_SOURCEDIR,
              "--mcs=5 --referenceSnrDb=20 --packetSize=1200 --eesmBetaLinear=2")
    {
    }

    std::string GetPostProcessingCommand() const override
    {
        return "| sed '/^Env: /d'";
    }
};

class SionnaSuMimoMinimalSuite : public ns3::TestSuite
{
  public:
    SionnaSuMimoMinimalSuite()
        : TestSuite("sionna-example-sumimo-minimal", EXAMPLE)
    {
        AddTestCase(new SionnaSuMimoMinimalTest, QUICK);
    }
};

static SionnaSuMimoMinimalSuite g_sionnaSuMimoMinimalSuite;

class SionnaMimoHelperConfigurationFailureTest : public ns3::ExampleAsTestCase
{
  public:
    SionnaMimoHelperConfigurationFailureTest(const std::string& name,
                                              const std::string& failureCase)
        : ExampleAsTestCase(name,
                            "ns3_wifi_sumimo_minimal",
                            NS_TEST_SOURCEDIR,
                            "--mcs=5 --eesmBetaLinear=2 --failureCase=" + failureCase,
                            false)
    {
    }

    std::string GetPostProcessingCommand() const override
    {
        return "| sed -e '/^Env: /d' -e '/^Command /d'";
    }
};

class SionnaMimoHelperConfigurationNegativeSuite : public ns3::TestSuite
{
  public:
    SionnaMimoHelperConfigurationNegativeSuite()
        : TestSuite("sionna-example-mimo-helper-configuration-negative", EXAMPLE)
    {
        AddTestCase(new SionnaMimoHelperConfigurationFailureTest(
                        "sionna-example-mimo-helper-invalid-nss",
                        "invalid-nss"),
                    QUICK);
        AddTestCase(new SionnaMimoHelperConfigurationFailureTest(
                        "sionna-example-mimo-helper-unsupported-mode",
                        "unsupported-mode"),
                    QUICK);
        AddTestCase(new SionnaMimoHelperConfigurationFailureTest(
                        "sionna-example-mimo-helper-missing-beta",
                        "missing-beta"),
                    QUICK);
    }
};

static SionnaMimoHelperConfigurationNegativeSuite
    g_sionnaMimoHelperConfigurationNegativeSuite;

class SionnaMimoHelperPhyMismatchTest : public ns3::ExampleAsTestCase
{
  public:
    SionnaMimoHelperPhyMismatchTest()
        : ExampleAsTestCase("sionna-example-mimo-helper-phy-cfr-mismatch",
                            "ns3_wifi_sumimo_minimal",
                            NS_TEST_SOURCEDIR,
                            "--mcs=5 --eesmBetaLinear=2 --failureCase=phy-cfr-mismatch",
                            false)
    {
    }

    std::string GetPostProcessingCommand() const override
    {
        return "| sed -E -e '/^Env: /d' -e 's#file=[^,]*sionna-spectrum-propagation-loss-model.cc, "
               "line=[0-9]+#file=sionna-spectrum-propagation-loss-model.cc, "
               "line=<runtime-guard>#'";
    }
};

class SionnaMimoHelperRuntimeNegativeSuite : public ns3::TestSuite
{
  public:
    SionnaMimoHelperRuntimeNegativeSuite()
        : TestSuite("sionna-example-mimo-helper-runtime-negative", EXAMPLE)
    {
        AddTestCase(new SionnaMimoHelperPhyMismatchTest, QUICK);
    }
};

static SionnaMimoHelperRuntimeNegativeSuite g_sionnaMimoHelperRuntimeNegativeSuite;
