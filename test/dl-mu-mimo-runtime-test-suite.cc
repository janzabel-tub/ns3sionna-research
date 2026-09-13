/* SPDX-License-Identifier: GPL-2.0-only */

#include "ns3/dl-mu-mimo-runtime.h"
#include "ns3/packet.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/test.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-psdu.h"

using namespace ns3;

namespace
{

MultiUserChannel
MakeUser(uint32_t id,
         const std::complex<double>& first,
         const std::complex<double>& second,
         uint32_t apNodeId,
         Time csiTimestamp)
{
    MimoChannelMatrix channel(1, 1, 2);
    channel.Set(0, 0, 0, first);
    channel.Set(0, 0, 1, second);
    return {id,
            apNodeId,
            id,
            MultiUserChannelDirection::DOWNLINK,
            csiTimestamp,
            {1, 1, 0.5, 0.5, "dipole", "V"},
            {0},
            0.0,
            channel};
}

MultiUserChannelSnapshot
MakeSnapshot(Time csiTimestamp = NanoSeconds(0), uint32_t apNodeId = 42)
{
    return {csiTimestamp,
            MultiUserChannelDirection::DOWNLINK,
            {MakeUser(3, {1.0, 0.0}, {0.0, 0.0}, apNodeId, csiTimestamp),
             MakeUser(9, {1.0, 0.0}, {0.0, 0.0}, apNodeId, csiTimestamp),
             MakeUser(12, {0.0, 0.0}, {1.0, 0.0}, apNodeId, csiTimestamp)}};
}

MultiUserScheduler::DlMuInfo
MakeInfo(const std::vector<uint16_t>& selectedStaIds = {7, 11, 15})
{
    MultiUserScheduler::DlMuInfo info;
    auto& tx = info.txParams.m_txVector;
    tx.SetPreambleType(WIFI_PREAMBLE_HE_MU);
    tx.SetChannelWidth(20);
    tx.SetGuardInterval(800);
    const HeRu::RuSpec ru{HeRu::RU_242_TONE, 1, true};
    WifiMacHeader header;
    header.SetType(WIFI_MAC_QOSDATA);
    for (const auto staId : selectedStaIds)
    {
        tx.SetHeMuUserInfo(staId, {ru, 0, 1});
        info.psduMap.emplace(staId, Create<WifiPsdu>(Create<Packet>(1), header));
    }
    return info;
}

std::vector<DlMuMimoStaUserBinding>
Bindings()
{
    return {{11, 9}, {15, 12}, {7, 3}};
}

bool
SamePrecoder(const DlMuMimoPrecodingMatrix& first, const DlMuMimoPrecodingMatrix& second)
{
    return first.GetPrecodingMatrix().GetCoefficients() ==
           second.GetPrecodingMatrix().GetCoefficients();
}

class DlMuMimoRuntimeSelectionTest : public TestCase
{
  public:
    DlMuMimoRuntimeSelectionTest()
        : TestCase("DL MU runtime deterministically selects and retains exact HE scheduler state")
    {
    }

  private:
    void DoRun() override
    {
        DlMuMimoRuntimeConfiguration configuration;
        configuration.groupingMode = DlMuMimoGroupingMode::MINIMUM_CORRELATION;
        configuration.requestedGroupSize = 2;
        configuration.timing.feedbackDelay = NanoSeconds(100);
        DlMuMimoRuntime runtime(configuration);
        const auto first = runtime.SelectAndStore(42,
                                                  MakeSnapshot(),
                                                  MakeInfo(),
                                                  Bindings(),
                                                  NanoSeconds(100));
        const auto second = runtime.SelectAndStore(42,
                                                   MakeSnapshot(),
                                                   MakeInfo(),
                                                   {{7, 3}, {11, 9}, {15, 12}},
                                                   NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(first.GetUsers().at(0).userId, 3, "stable tie break changed");
        NS_TEST_ASSERT_MSG_EQ(first.GetUsers().at(1).userId, 12, "orthogonal pair was not selected");
        NS_TEST_ASSERT_MSG_EQ(first.GetUsers().at(0).staId,
                              7,
                              "scheduler STA-ID assignment changed");
        NS_TEST_ASSERT_MSG_EQ(first.GetUsers().at(0).nss,
                              1,
                              "scheduler NSS assignment changed");
        NS_TEST_ASSERT_MSG_EQ(first.GetUsers().at(0).ru,
                              first.GetUsers().at(1).ru,
                              "selected spatial group did not retain its common RU");
        NS_TEST_ASSERT_MSG_EQ_TOL(first.GetGroupingMetric(), 0.0, 1e-15, "wrong correlation metric");
        NS_TEST_ASSERT_MSG_EQ(first.GetCsiTimestamp(), NanoSeconds(0), "wrong resolved CSI time");
        NS_TEST_ASSERT_MSG_EQ(first.GetCsiAge(), NanoSeconds(100), "wrong CSI age");
        NS_TEST_ASSERT_MSG_EQ(first.GetFeedbackDelay(),
                              NanoSeconds(100),
                              "feedback delay diagnostic changed");
        NS_TEST_ASSERT_MSG_EQ_TOL(first.GetPrecoding().GetPrecodingMatrix().GetPower(0),
                                  2.0,
                                  1e-12,
                                  "runtime ZF changed total AP power");
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(first.GetPrecoding(), second.GetPrecoding()), true,
                              "repeated selection changed precoding");

        // The scheduler consumes GetUsers() and emits this exact selected assignment.
        const auto selectedInfo = MakeInfo({7, 15});
        NS_TEST_ASSERT_MSG_EQ(selectedInfo.psduMap.size(), 2u, "selected PSDU map was not filtered");
        NS_TEST_ASSERT_MSG_EQ(selectedInfo.txParams.m_txVector.GetHeMuUserInfoMap().size(),
                              2u,
                              "selected TxVector map was not filtered");
        const auto& retained = runtime.Lookup(42, selectedInfo, {{7, 3}, {15, 12}}, NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(retained.GetPrecoding(), first.GetPrecoding()), true,
                              "exact state lookup changed precoding");
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        runtime.Install(*adapter, 42, selectedInfo, {{7, 3}, {15, 12}}, NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(), true, "runtime install did not retain state");

        auto changedCorrelation = MakeSnapshot();
        changedCorrelation.users[0].channel.Set(0, 0, 0, {0.0, 0.0});
        changedCorrelation.users[0].channel.Set(0, 0, 1, {1.0, 0.0});
        DlMuMimoRuntime changedRuntime(configuration);
        const auto changed = changedRuntime.SelectAndStore(42,
                                                           changedCorrelation,
                                                           MakeInfo(),
                                                           Bindings(),
                                                           NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(changed.GetUsers().at(0).userId,
                              3,
                              "changed-correlation stable order changed");
        NS_TEST_ASSERT_MSG_EQ(changed.GetUsers().at(1).userId,
                              9,
                              "controlled correlation change did not change the group");

        auto periodicConfiguration = configuration;
        periodicConfiguration.timing.updateInterval = NanoSeconds(50);
        periodicConfiguration.timing.feedbackDelay = NanoSeconds(20);
        DlMuMimoRuntime periodicRuntime(periodicConfiguration);
        const auto periodic = periodicRuntime.SelectAndStore(42,
                                                             MakeSnapshot(NanoSeconds(100)),
                                                             MakeInfo(),
                                                             Bindings(),
                                                             NanoSeconds(145));
        NS_TEST_ASSERT_MSG_EQ(periodic.GetCsiAge(),
                              NanoSeconds(45),
                              "periodic CSI age was not retained");
        NS_TEST_ASSERT_MSG_EQ(periodic.GetUpdateInterval(),
                              NanoSeconds(50),
                              "CSI update interval diagnostic changed");
        NS_TEST_ASSERT_MSG_EQ(periodic.GetFeedbackDelay(),
                              NanoSeconds(20),
                              "periodic feedback delay diagnostic changed");

        DlMuMimoRuntimeConfiguration rzfConfiguration;
        rzfConfiguration.requestedUserIds = {12, 3};
        rzfConfiguration.precoder = DlMuMimoRuntimePrecoder::REGULARIZED_ZERO_FORCING;
        rzfConfiguration.rzfAlpha = 0.25;
        DlMuMimoRuntime rzfRuntime(rzfConfiguration);
        const auto rzf = rzfRuntime.SelectAndStore(42,
                                                   MakeSnapshot(),
                                                   MakeInfo(),
                                                   Bindings(),
                                                   NanoSeconds(0));
        const auto expectedRzf = DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
            rzf.GetSnapshot(),
            {{12, 0}, {3, 0}},
            0.25);
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(rzf.GetPrecoding(), expectedRzf),
                              true,
                              "runtime RZF differs from the Phase-4.3 factory");
    }
};

class DlMuMimoRuntimeRejectionTest : public TestCase
{
  public:
    DlMuMimoRuntimeRejectionTest()
        : TestCase("DL MU runtime rejects invalid state without changing retained selection")
    {
    }

  private:
    void DoRun() override
    {
        auto expectRejected = [this](const auto& operation, const std::string& reason) {
            bool rejected = false;
            try
            {
                operation();
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };

        DlMuMimoRuntimeConfiguration config;
        config.requestedUserIds = {3, 12};
        config.timing.feedbackDelay = NanoSeconds(100);
        DlMuMimoRuntime runtime(config);
        const auto state = runtime.SelectAndStore(42,
                                                  MakeSnapshot(),
                                                  MakeInfo(),
                                                  Bindings(),
                                                  NanoSeconds(100));
        auto invalid = config;
        invalid.feedback = DlMuMimoFeedbackRepresentation::QUANTIZED_CHANNEL;
        expectRejected(
            [&]() { static_cast<void>(DlMuMimoRuntime(invalid)); },
            "quantized MU feedback was accepted");
        invalid.feedback = DlMuMimoFeedbackRepresentation::SU_PRECODER_RECONSTRUCTION;
        expectRejected(
            [&]() { static_cast<void>(DlMuMimoRuntime(invalid)); },
            "an SU reconstructed precoder was accepted as MU channel feedback");

        auto eht = MakeInfo();
        eht.txParams.m_txVector.SetPreambleType(WIFI_PREAMBLE_EHT_MU);
        expectRejected(
            [&]() { runtime.SelectAndStore(42, MakeSnapshot(), eht, Bindings(), NanoSeconds(100)); },
            "EHT scheduler state was accepted");

        auto uplink = MakeSnapshot();
        uplink.direction = MultiUserChannelDirection::UPLINK;
        for (auto& user : uplink.users)
        {
            user.direction = MultiUserChannelDirection::UPLINK;
        }
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       uplink,
                                       MakeInfo(),
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "uplink CSI was accepted");
        expectRejected(
            [&]() {
                runtime.SelectAndStore(43,
                                       MakeSnapshot(),
                                       MakeInfo(),
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "a snapshot for another AP was accepted");
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       MakeSnapshot(),
                                       MakeInfo(),
                                       {{7, 3}, {11, 9}},
                                       NanoSeconds(100));
            },
            "a missing STA-ID/user-ID binding was accepted");

        auto differentRu = MakeInfo();
        differentRu.txParams.m_txVector.SetHeMuUserInfo(
            15,
            {{HeRu::RU_106_TONE, 1, true}, 0, 1});
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       MakeSnapshot(),
                                       differentRu,
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "a fixed group spanning different RUs was accepted");

        auto multipleStreams = MakeInfo();
        multipleStreams.txParams.m_txVector.SetNss(2, 15);
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       MakeSnapshot(),
                                       multipleStreams,
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "multiple streams per user were accepted");

        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       MakeSnapshot(NanoSeconds(1)),
                                       MakeInfo(),
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "an off-grid CSI timestamp was accepted");

        auto future = MakeSnapshot(NanoSeconds(101));
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       future,
                                       MakeInfo(),
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "future CSI was accepted");

        auto rankDeficientConfig = config;
        rankDeficientConfig.requestedUserIds = {3, 9};
        DlMuMimoRuntime rankDeficientRuntime(rankDeficientConfig);
        expectRejected(
            [&]() {
                rankDeficientRuntime.SelectAndStore(42,
                                                    MakeSnapshot(),
                                                    MakeInfo(),
                                                    Bindings(),
                                                    NanoSeconds(100));
            },
            "a rank-deficient fixed group was accepted");

        auto overloadedConfig = config;
        overloadedConfig.requestedUserIds = {3, 9, 12};
        DlMuMimoRuntime overloadedRuntime(overloadedConfig);
        expectRejected(
            [&]() {
                overloadedRuntime.SelectAndStore(42,
                                                 MakeSnapshot(),
                                                 MakeInfo(),
                                                 Bindings(),
                                                 NanoSeconds(100));
            },
            "an overloaded group was accepted");

        expectRejected(
            [&]() {
                runtime.Lookup(42,
                               MakeInfo({7, 11}),
                               {{7, 3}, {11, 9}},
                               NanoSeconds(100));
            },
            "runtime state was reused for another user group");
        expectRejected(
            [&]() {
                runtime.Lookup(42,
                               MakeInfo({7, 15}),
                               {{7, 3}, {15, 12}},
                               NanoSeconds(101));
            },
            "runtime state was reused at another transmission time");

        const auto& stillThere = runtime.Lookup(42,
                                                MakeInfo({7, 15}),
                                                {{7, 3}, {15, 12}},
                                                NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(stillThere.GetPrecoding(), state.GetPrecoding()),
                              true,
                              "rejected input altered valid state");

        const auto newer = runtime.SelectAndStore(42,
                                                  MakeSnapshot(NanoSeconds(100)),
                                                  MakeInfo(),
                                                  Bindings(),
                                                  NanoSeconds(200));
        expectRejected(
            [&]() {
                runtime.SelectAndStore(42,
                                       MakeSnapshot(),
                                       MakeInfo(),
                                       Bindings(),
                                       NanoSeconds(100));
            },
            "older CSI replaced newer state");
        const auto& newerStillThere = runtime.Lookup(42,
                                                     MakeInfo({7, 15}),
                                                     {{7, 3}, {15, 12}},
                                                     NanoSeconds(200));
        NS_TEST_ASSERT_MSG_EQ(
            SamePrecoder(newerStillThere.GetPrecoding(), newer.GetPrecoding()),
            true,
            "rejected older replacement altered newer state");
    }
};

class DlMuMimoRuntimeFixedTransmissionTest : public TestCase
{
  public:
    DlMuMimoRuntimeFixedTransmissionTest()
        : TestCase("DL MU runtime retains fixed precoding and atomically installs a distinct transmission channel")
    {
    }

  private:
    void DoRun() override
    {
        const DlMuMimoPrecodingMatrix fixed(
            MimoPrecodingMatrix::FrequencyFlat(1, 2, 2, {{1.0, 0.0}, {0.0, 0.0},
                                                          {0.0, 0.0}, {1.0, 0.0}}),
            {{3, 0}, {12, 0}});
        DlMuMimoRuntimeConfiguration configuration;
        configuration.requestedUserIds = {3, 12};
        configuration.precoder = DlMuMimoRuntimePrecoder::FIXED;
        configuration.fixedPrecoding = fixed;
        configuration.timing.feedbackDelay = NanoSeconds(100);
        DlMuMimoRuntime runtime(configuration);
        const auto selection = runtime.SelectAndStore(42,
                                                      MakeSnapshot(),
                                                      MakeInfo(),
                                                      Bindings(),
                                                      NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(selection.GetPrecoding(), fixed),
                              true,
                              "fixed runtime coefficients changed during selection");

        auto transmission = selection.GetSnapshot();
        transmission.csiTimestamp = NanoSeconds(100);
        for (auto& user : transmission.users)
        {
            user.csiTimestamp = NanoSeconds(100);
            user.widebandLossDb += 3.0;
            user.channel.Set(0, 0, 0, user.channel.Get(0, 0, 0) + std::complex<double>(0.25, 0.0));
        }
        const auto selectedInfo = MakeInfo({7, 15});
        const std::vector<DlMuMimoStaUserBinding> selectedBindings{{7, 3}, {15, 12}};
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        runtime.Install(*adapter,
                        42,
                        selectedInfo,
                        selectedBindings,
                        transmission,
                        NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(), true,
                              "fixed runtime install did not retain MU state");
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDownlinkTransmissionSnapshot(), true,
                              "fixed runtime install did not retain the physical channel");
        NS_TEST_ASSERT_MSG_EQ(SamePrecoder(runtime.Lookup(42,
                                                        selectedInfo,
                                                        selectedBindings,
                                                        NanoSeconds(100))
                                                .GetPrecoding(),
                                      fixed),
                              true,
                              "transmission-channel install changed retained fixed precoding");
        NS_TEST_ASSERT_MSG_EQ(selection.GetSnapshot().csiTimestamp,
                              NanoSeconds(0),
                              "transmission-channel install changed retained CSI timestamp");

        auto expectRejected = [this](const auto& operation, const std::string& reason) {
            bool rejected = false;
            try
            {
                operation();
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, reason);
        };
        auto wrongTimestamp = transmission;
        wrongTimestamp.csiTimestamp = NanoSeconds(99);
        for (auto& user : wrongTimestamp.users)
        {
            user.csiTimestamp = NanoSeconds(99);
        }
        expectRejected(
            [&]() {
                runtime.Install(*adapter,
                                42,
                                selectedInfo,
                                selectedBindings,
                                wrongTimestamp,
                                NanoSeconds(100));
            },
            "a transmission snapshot with another timestamp was accepted");
        auto wrongEndpoint = transmission;
        wrongEndpoint.users.front().rxNodeId = 99;
        expectRejected(
            [&]() {
                runtime.Install(*adapter,
                                42,
                                selectedInfo,
                                selectedBindings,
                                wrongEndpoint,
                                NanoSeconds(100));
            },
            "a transmission snapshot with another endpoint was accepted");
        auto wrongGrid = transmission;
        wrongGrid.users.front().frequencies = {1};
        expectRejected(
            [&]() {
                runtime.Install(*adapter,
                                42,
                                selectedInfo,
                                selectedBindings,
                                wrongGrid,
                                NanoSeconds(100));
            },
            "a transmission snapshot with another grid was accepted");
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(), true,
                              "an invalid transmission snapshot cleared MU state");
        NS_TEST_ASSERT_MSG_EQ(adapter->HasDownlinkTransmissionSnapshot(), true,
                              "an invalid transmission snapshot cleared physical channel state");

        auto missingFixed = configuration;
        missingFixed.fixedPrecoding.reset();
        expectRejected([&]() { static_cast<void>(DlMuMimoRuntime(missingFixed)); },
                       "fixed precoding without a matrix was accepted");
        auto incompatibleFixed = configuration;
        incompatibleFixed.groupingMode = DlMuMimoGroupingMode::MINIMUM_CORRELATION;
        incompatibleFixed.requestedUserIds.clear();
        incompatibleFixed.requestedGroupSize = 2;
        expectRejected([&]() { static_cast<void>(DlMuMimoRuntime(incompatibleFixed)); },
                       "fixed precoding with correlation grouping was accepted");
    }
};

class DlMuMimoRuntimeTestSuite : public TestSuite
{
  public:
    DlMuMimoRuntimeTestSuite()
        : TestSuite("dl-mu-mimo-runtime", UNIT)
    {
        AddTestCase(new DlMuMimoRuntimeSelectionTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoRuntimeRejectionTest, TestCase::QUICK);
        AddTestCase(new DlMuMimoRuntimeFixedTransmissionTest, TestCase::QUICK);
    }
};

static DlMuMimoRuntimeTestSuite g_dlMuMimoRuntimeTestSuite;

} // namespace
