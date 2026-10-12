/* SPDX-License-Identifier: GPL-2.0-only */

#include "ns3/ctrl-headers.h"
#include "ns3/he-phy.h"
#include "ns3/he-ppdu.h"
#include "ns3/he-ul-mu-mimo-phy-adapter.h"
#include "ns3/mimo-channel-matrix.h"
#include "ns3/packet.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/test.h"
#include "ns3/simulator.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-phy-operating-channel.h"
#include "ns3/wifi-psdu.h"

#include <cmath>
#include <map>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ns3
{
namespace
{

constexpr uint32_t AP_NODE_ID = 100;
constexpr uint64_t TRIGGER_UID = 77;
constexpr double BOLTZMANN = 1.3803e-23;
const Time COORDINATED_START = MicroSeconds(100);

MultiUserChannel
MakeUser(uint32_t userId, uint32_t txNodeId, const MimoChannelMatrix& channel)
{
    return {userId,
            txNodeId,
            AP_NODE_ID,
            MultiUserChannelDirection::UPLINK,
            NanoSeconds(1234),
            {channel.GetNumRxAntennas(), 1, 0.5, 0.5, "dipole", "V"},
            {0},
            0.0,
            channel};
}

MultiUserChannelSnapshot
MakeSnapshot(const std::vector<MultiUserChannel>& users)
{
    return {NanoSeconds(1234), MultiUserChannelDirection::UPLINK, users};
}

WifiTxVector
MakeTriggerVector(const std::vector<std::tuple<uint16_t, HeRu::RuSpec, uint8_t>>& users,
                  Time requestedDuration,
                  Time& duration, uint16_t width = 20)
{
    WifiTxVector vector;
    vector.SetPreambleType(WIFI_PREAMBLE_HE_TB);
    vector.SetChannelWidth(width);
    vector.SetGuardInterval(1600);
    for (const auto& [staId, ru, mcs] : users)
    {
        vector.SetHeMuUserInfo(staId, {ru, mcs, 1});
    }
    const auto converted =
        HePhy::ConvertHeTbPpduDurationToLSigLength(requestedDuration,
                                                   vector,
                                                   WIFI_PHY_BAND_5GHZ);
    vector.SetLength(converted.first);
    duration = converted.second;
    return vector;
}

Ptr<const HePpdu>
MakePpdu(const CtrlTriggerHeader& trigger, uint16_t staId, uint32_t bytes, Time duration,
         uint64_t uid = TRIGGER_UID, uint16_t width = 20, uint8_t primary20 = 0)
{
    WifiMacHeader header;
    header.SetType(WIFI_MAC_QOSDATA);
    header.SetQosTid(0);
    header.SetAddr1(Mac48Address("00:00:00:00:00:01"));
    header.SetAddr2(staId == 7 ? Mac48Address("00:00:00:00:00:07")
                              : Mac48Address("00:00:00:00:00:09"));
    WifiConstPsduMap psdus{{staId, Create<WifiPsdu>(Create<Packet>(bytes), header)}};
    static std::map<std::pair<uint16_t, uint8_t>, WifiPhyOperatingChannel> channels;
    const auto inserted = channels.try_emplace({width, primary20});
    auto& channel = inserted.first->second;
    if (inserted.second)
    {
        channel.SetDefault(width, WIFI_STANDARD_80211ax, WIFI_PHY_BAND_5GHZ);
        channel.SetPrimary20Index(primary20);
    }
    return Create<HePpdu>(psdus,
                          trigger.GetHeTbTxVector(staId),
                          channel,
                          duration,
                          uid,
                          HePpdu::PSD_HE_PORTION);
}

double
ReceivedPowerForSnr(double snr, const HeRu::RuSpec& ru)
{
    return snr * BOLTZMANN * 290.0 * HeRu::GetBandwidth(ru.GetRuType()) * 1e6;
}

Ptr<HeUlMuMimoPhyAdapter>
MakeAdapter()
{
    auto adapter = CreateObject<HeUlMuMimoPhyAdapter>();
        adapter->SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel::EESM);
    adapter->SetErrorRateModel(CreateObject<TableBasedErrorRateModel>());
    adapter->SetEesmBeta(0, 2.0);
    adapter->SetEesmBeta(3, 3.0);
    return adapter;
}

class HeUlBerPhysicalMaskTest : public TestCase
{
  public:
    HeUlBerPhysicalMaskTest() : TestCase("General UL MU filters data tones before per-user BER mapping") {}
  private:
    void DoRun() override
    {
        RunCase(20, 0);
        RunCase(160, 0);
        RunCase(160, 4);
    }
    void RunCase(uint16_t width, uint8_t primary20)
    {
        const unsigned n = 3 * width * 256 / 20;
        const int shift = width == 160 ? (primary20 < 4 ? -512 : 512) : 0;
        const HeRu::RuSpec ru(width == 20 ? HeRu::RU_242_TONE : HeRu::RU_996_TONE,1,true);
        Time duration;
        const CtrlTriggerHeader trigger(TriggerFrameType::BASIC_TRIGGER,
            MakeTriggerVector({{7,ru,0},{9,ru,3}},MicroSeconds(200),duration,width));
        MimoChannelMatrix h1(n,2,1), h2(n,2,1);
        std::vector<double> samples1, samples2;
        for (unsigned k = 0; k < n; ++k)
        {
            const int tone = int(k)-int(n/2)-shift;
            const int a = std::abs(tone);
            const bool data = width == 20 ?
                a >= 2 && a <= 122 && a != 22 && a != 48 && a != 90 && a != 116 :
                a >= 3 && a <= 500 && a != 24 && a != 92 && a != 158 && a != 226 &&
                a != 266 && a != 334 && a != 400 && a != 468;
            const double gain = tone < 0 ? 0.1 : 2.0;
            h1.Set(k,0,0,{data ? std::sqrt(gain) : 100,0});
            h2.Set(k,1,0,{data ? 1.0 : 100,0});
            if (data) { samples1.push_back(4*gain); samples2.push_back(9); }
        }
        auto snapshot = MakeSnapshot({MakeUser(1,11,h1),MakeUser(2,22,h2)});
        for (auto& user : snapshot.users)
        {
            user.frequencies.resize(n);
            for (unsigned k = 0; k < n; ++k) { user.frequencies[k] = int(k) - int(n/2); }
        }
        const auto end = COORDINATED_START + duration;
        const std::vector<HeUlMuMimoPpduReception> receptions{
            {7,11,MakePpdu(trigger,7,400,duration,TRIGGER_UID,width,primary20),COORDINATED_START,end,ReceivedPowerForSnr(4,ru),400,{}},
            {9,22,MakePpdu(trigger,9,900,duration,TRIGGER_UID,width,primary20),COORDINATED_START,end,ReceivedPowerForSnr(9,ru),900,{}}};
        auto adapter = CreateObject<HeUlMuMimoPhyAdapter>();
        adapter->SetErrorRateModel(CreateObject<TableBasedErrorRateModel>());
        const auto result = adapter->Calculate(snapshot,trigger,TRIGGER_UID,{{1,7,11},{2,9,22}},
            receptions,COORDINATED_START,end,AP_NODE_ID,2,0,UlMuMimoDetector::LINEAR_MMSE,
            UlMuMimoLinkModel::DEFAULT_RANK_TOLERANCE,primary20);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetUserById(1).GetEffectiveSinrLinear(),
            HeEffectiveSinrMapping::MeanUncodedBer(samples1,0).effectiveSinrLinear,1e-11,"UL pilot/null contamination");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetUserById(2).GetEffectiveSinrLinear(),9,1e-11,"UL per-user modulation mapping");
        const auto& user = result.GetUserById(2);
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEquivalentReceivedPowerW(2),
            9*user.GetLinkBudget().GetReceiverNoisePowerW()/2,1e-24,"UL duplicated diversity gain");
        Simulator::Destroy();
    }
};
class HeUlMuMimoPhyAdapterIntegrationTest : public TestCase
{
  public:
    HeUlMuMimoPhyAdapterIntegrationTest()
        : TestCase("HE Basic Trigger and simultaneous HE-TB PPDUs drive UL MU detection")
    {
    }

  private:
    void DoRun() override
    {
        const MimoChannelMatrix user1(1, 2, 1, {{1.0, 0.0}, {0.0, 0.0}});
        const MimoChannelMatrix user2(1, 2, 1, {{0.0, 0.0}, {1.0, 0.0}});
        const auto snapshot =
            MakeSnapshot({MakeUser(1, 11, user1), MakeUser(2, 22, user2)});
        const HeRu::RuSpec fullRu(HeRu::RU_242_TONE, 1, true);
        Time duration;
        const auto triggerVector =
            MakeTriggerVector({{7, fullRu, 0}, {9, fullRu, 3}}, MicroSeconds(200), duration);
        const CtrlTriggerHeader trigger(TriggerFrameType::BASIC_TRIGGER, triggerVector);
        const Time end = COORDINATED_START + duration;
        const std::vector<HeUlMuMimoUserBinding> bindings{{1, 7, 11}, {2, 9, 22}};
        const std::vector<HeUlMuMimoPpduReception> receptions{
            {9,
             22,
             MakePpdu(trigger, 9, 900, duration),
             COORDINATED_START,
             end,
             ReceivedPowerForSnr(9.0, fullRu),
             900,
             {0}},
            {7,
             11,
             MakePpdu(trigger, 7, 400, duration),
             COORDINATED_START,
             end,
             ReceivedPowerForSnr(4.0, fullRu),
             400,
             {0}}};

        const auto result = MakeAdapter()->Calculate(snapshot,
                                                     trigger,
                                                     TRIGGER_UID,
                                                     bindings,
                                                     receptions,
                                                     COORDINATED_START,
                                                     end,
                                                     AP_NODE_ID,
                                                     2,
                                                     0.0,
                                                     UlMuMimoDetector::LINEAR_MMSE);
        NS_TEST_ASSERT_MSG_EQ(result.GetNumGroups(), 1, "Shared RU did not form one group");
        NS_TEST_ASSERT_MSG_EQ(result.GetNumDetectedUsers(), 2, "A simultaneous user was lost");
        NS_TEST_ASSERT_MSG_EQ(result.GetMissingStaIds().empty(), true, "User reported missing");
        NS_TEST_ASSERT_MSG_EQ(result.GetNonSimultaneousStaIds().empty(),
                              true,
                              "User reported non-simultaneous");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetUserById(1).GetEffectiveSinrLinear(),
                                  4.0,
                                  1e-10,
                                  "User 1 trigger mapping or SINR is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetUserById(2).GetEffectiveSinrLinear(),
                                  9.0,
                                  1e-10,
                                  "User 2 trigger mapping or SINR is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(
            result.GetUserById(1).GetMimoResult().GetInterUserInterferencePower(0, 0),
            0.0,
            1e-12,
            "Orthogonal simultaneous users interfere");
        const double receiverNoise = BOLTZMANN * 290.0 * 20e6;
        NS_TEST_ASSERT_MSG_EQ_TOL(result.GetEquivalentReceivedPowerW(1),
                                  4.0 * receiverNoise / 2.0,
                                  1e-20,
                                  "Scalar PHY handoff applied AP diversity incorrectly");

        // A second RU is orthogonal even when its PPDU is exactly coincident.
        const HeRu::RuSpec leftRu(HeRu::RU_106_TONE, 1, true);
        const HeRu::RuSpec rightRu(HeRu::RU_106_TONE, 2, true);
        Time splitDuration;
        const CtrlTriggerHeader splitTrigger(
            TriggerFrameType::BASIC_TRIGGER,
            MakeTriggerVector({{7, leftRu, 0}, {9, rightRu, 3}},
                              MicroSeconds(200),
                              splitDuration));
        const Time splitEnd = COORDINATED_START + splitDuration;
        const std::vector<HeUlMuMimoPpduReception> splitReceptions{
            {7,
             11,
             MakePpdu(splitTrigger, 7, 400, splitDuration),
             COORDINATED_START,
             splitEnd,
             ReceivedPowerForSnr(4.0, leftRu),
             400,
             {0}},
            {9,
             22,
             MakePpdu(splitTrigger, 9, 900, splitDuration),
             COORDINATED_START,
             splitEnd,
             ReceivedPowerForSnr(9.0, rightRu),
             900,
             {0}}};
        const auto split = MakeAdapter()->Calculate(snapshot,
                                                    splitTrigger,
                                                    TRIGGER_UID,
                                                    bindings,
                                                    splitReceptions,
                                                    COORDINATED_START,
                                                    splitEnd,
                                                    AP_NODE_ID,
                                                    2,
                                                    0.0,
                                                    UlMuMimoDetector::ZERO_FORCING);
        NS_TEST_ASSERT_MSG_EQ(split.GetNumGroups(), 2, "Distinct RUs were jointly detected");
        NS_TEST_ASSERT_MSG_EQ_TOL(split.GetUserById(1).GetEffectiveSinrLinear(),
                                  4.0,
                                  1e-10,
                                  "Left-RU user SINR mismatch");
        NS_TEST_ASSERT_MSG_EQ_TOL(split.GetUserById(2).GetEffectiveSinrLinear(),
                                  9.0,
                                  1e-10,
                                  "Right-RU user SINR mismatch");
    }
};

class HeUlMuMimoPhyAdapterParticipationTest : public TestCase
{
  public:
    HeUlMuMimoPhyAdapterParticipationTest()
        : TestCase("missing, late, and inconsistent HE-TB participants are explicit")
    {
    }

  private:
    void DoRun() override
    {
        const MimoChannelMatrix user1(1, 2, 1, {{1.0, 0.0}, {0.0, 0.0}});
        const MimoChannelMatrix user2(1, 2, 1, {{0.0, 0.0}, {1.0, 0.0}});
        const auto snapshot =
            MakeSnapshot({MakeUser(1, 11, user1), MakeUser(2, 22, user2)});
        const HeRu::RuSpec ru(HeRu::RU_242_TONE, 1, true);
        Time duration;
        const CtrlTriggerHeader trigger(
            TriggerFrameType::BASIC_TRIGGER,
            MakeTriggerVector({{7, ru, 0}, {9, ru, 3}}, MicroSeconds(200), duration));
        const Time end = COORDINATED_START + duration;
        const std::vector<HeUlMuMimoUserBinding> bindings{{1, 7, 11}, {2, 9, 22}};

        // STA 7 is absent and STA 9 arrives one nanosecond late. Neither enters a detector.
        const std::vector<HeUlMuMimoPpduReception> incomplete{
            {9,
             22,
             MakePpdu(trigger, 9, 900, duration),
             COORDINATED_START + NanoSeconds(1),
             end + NanoSeconds(1),
             ReceivedPowerForSnr(9.0, ru),
             900,
             {0}}};
        const auto result = MakeAdapter()->Calculate(snapshot,
                                                     trigger,
                                                     TRIGGER_UID,
                                                     bindings,
                                                     incomplete,
                                                     COORDINATED_START,
                                                     end,
                                                     AP_NODE_ID,
                                                     2,
                                                     0.0,
                                                     UlMuMimoDetector::LINEAR_MMSE);
        NS_TEST_ASSERT_MSG_EQ(result.GetNumDetectedUsers(), 0, "Absent or late user detected");
        NS_TEST_ASSERT_MSG_EQ(result.GetMissingStaIds().size(), 1, "Missing STA not reported");
        NS_TEST_ASSERT_MSG_EQ(result.GetMissingStaIds().front(), 7, "Wrong missing STA-ID");
        NS_TEST_ASSERT_MSG_EQ(result.GetNonSimultaneousStaIds().size(),
                              1,
                              "Late STA not reported");
        NS_TEST_ASSERT_MSG_EQ(result.GetNonSimultaneousStaIds().front(),
                              9,
                              "Wrong late STA-ID");

        bool wrongUidRejected = false;
        try
        {
            const std::vector<HeUlMuMimoPpduReception> wrongUid{
                {7,
                 11,
                 MakePpdu(trigger, 7, 400, duration, TRIGGER_UID + 1),
                 COORDINATED_START,
                 end,
                 ReceivedPowerForSnr(4.0, ru),
                 400,
                 {0}}};
            MakeAdapter()->Calculate(snapshot,
                                     trigger,
                                     TRIGGER_UID,
                                     bindings,
                                     wrongUid,
                                     COORDINATED_START,
                                     end,
                                     AP_NODE_ID,
                                     2,
                                     0.0,
                                     UlMuMimoDetector::LINEAR_MMSE);
        }
        catch (const std::invalid_argument&)
        {
            wrongUidRejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(wrongUidRejected,
                              true,
                              "HE-TB PPDU from another Trigger exchange was accepted");
    }
};

class HeUlMuMimoPhyAdapterTestSuite : public TestSuite
{
  public:
    HeUlMuMimoPhyAdapterTestSuite()
        : TestSuite("he-ul-mu-mimo-phy-adapter", UNIT)
    {
        AddTestCase(new HeUlBerPhysicalMaskTest, TestCase::QUICK);
        AddTestCase(new HeUlMuMimoPhyAdapterIntegrationTest, TestCase::QUICK);
        AddTestCase(new HeUlMuMimoPhyAdapterParticipationTest, TestCase::QUICK);
    }
};

static HeUlMuMimoPhyAdapterTestSuite g_heUlMuMimoPhyAdapterTestSuite;

} // namespace
} // namespace ns3
