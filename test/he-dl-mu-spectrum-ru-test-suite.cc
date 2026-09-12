/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/constant-position-mobility-model.h"
#include "ns3/double.h"
#include "ns3/he-phy.h"
#include "ns3/he-ppdu.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/simulator.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/spectrum-wifi-phy.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mac-helper.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-spectrum-phy-interface.h"
#include "ns3/wifi-spectrum-signal-parameters.h"
#include "ns3/wifi-spectrum-value-helper.h"
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>

using namespace ns3;
namespace
{
class HeDlMuSpectrumRuTest : public TestCase
{
  public:
    HeDlMuSpectrumRuTest() : TestCase("runtime RU tones exclude nulls and other allocations") {}

  private:
    void DoRun() override
    {
        // Hard-coded 20 MHz occupied-tone ranges are independent of the adapter's
        // HeRu/ConvertHeRuSubcarriers mapping. Include the disjoint centre 26 RU.
        for (const auto type : {HeRu::RU_242_TONE, HeRu::RU_106_TONE, HeRu::RU_26_TONE})
        {
            RunAllocation(type);
            Simulator::Destroy();
        }
    }

    void RunAllocation(HeRu::RuType type)
    {
        auto channel = CreateObject<MultiModelSpectrumChannel>();
        NodeContainer aps;
        NodeContainer stas;
        aps.Create(2);
        stas.Create(3);
        NodeContainer all(aps, stas);
        for (uint32_t i = 0; i < all.GetN(); ++i)
        {
            auto mobility = CreateObject<ConstantPositionMobilityModel>();
            all.Get(i)->AggregateObject(mobility);
            mobility->SetPosition({static_cast<double>(i), 0, 0});
        }
        SpectrumWifiPhyHelper apHelper;
        SpectrumWifiPhyHelper staHelper;
        for (auto helper : {&apHelper, &staHelper})
        {
            helper->SetChannel(channel);
            helper->Set("RxNoiseFigure", DoubleValue(7));
            helper->Set("RxGain", DoubleValue(0));
        }
        apHelper.Set("Antennas", UintegerValue(2));
        apHelper.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
        apHelper.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        WifiMacHelper apMac;
        WifiMacHelper staMac;
        apMac.SetType("ns3::ApWifiMac");
        staMac.SetType("ns3::StaWifiMac");
        auto apDevices = wifi.Install(apHelper, apMac, aps);
        auto staDevices = wifi.Install(staHelper, staMac, stas);
        auto apPhy = DynamicCast<SpectrumWifiPhy>(
            DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetPhy());
        apPhy->SetOperatingChannel({36, 20, WIFI_PHY_BAND_5GHZ, 0});
        // Reapply positions after installation, as in the original PPDU test.
        for (uint32_t i = 0; i < all.GetN(); ++i)
        {
            all.Get(i)->GetObject<MobilityModel>()->SetPosition({double(i), 0, 0});
        }
        WifiTxVector tx;
        tx.SetPreambleType(WIFI_PREAMBLE_HE_MU);
        tx.SetChannelWidth(20);
        tx.SetGuardInterval(800);
        tx.SetSigBMode(HePhy::GetVhtMcs(0));
        for (uint16_t u = 0; u < 2; ++u)
        {
            const uint8_t index = type == HeRu::RU_26_TONE ? 5 :
                                  type == HeRu::RU_106_TONE ? u + 1 : 1;
            tx.SetHeMuUserInfo(u + 1, {HeRu::RuSpec(type, index, true), 0, 1});
        }
        WifiConstPsduMap psdus;
        for (uint16_t u = 0; u < 2; ++u)
        {
            WifiMacHeader header;
            header.SetType(WIFI_MAC_QOSDATA);
            header.SetQosTid(0);
            header.SetAddr1(DynamicCast<WifiNetDevice>(staDevices.Get(u))->GetMac()->GetAddress());
            header.SetAddr2(DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetMac()->GetAddress());
            psdus.emplace(u + 1, Create<WifiPsdu>(Create<Packet>(400), header));
        }
        auto ppdu = Create<HePpdu>(psdus, tx, apPhy->GetOperatingChannel(),
                                   MicroSeconds(100), 1, HePpdu::PSD_HE_PORTION);
        auto psd = WifiSpectrumValueHelper::CreateHeOfdmTxPowerSpectralDensity(
            5180, 20, 1e-12, apPhy->GetGuardBandwidth(20));
        const uint32_t n = psd->GetValuesN() - 1;
        const int centre = n / 2;
        auto allocated = [type](int tone, uint32_t u) {
            if (type == HeRu::RU_242_TONE)
                return (tone >= -122 && tone <= -2) || (tone >= 2 && tone <= 122);
            if (type == HeRu::RU_26_TONE)
                return (tone >= -16 && tone <= -4) || (tone >= 4 && tone <= 16);
            return u == 0 ? tone >= -122 && tone <= -17 : tone >= 17 && tone <= 122;
        };
        std::vector<int> frequencies(n);
        std::iota(frequencies.begin(), frequencies.end(), -centre);
        MultiUserChannelSnapshot snapshot{NanoSeconds(0), MultiUserChannelDirection::DOWNLINK, {}};
        for (uint32_t u = 0; u < 2; ++u)
        {
            MimoChannelMatrix h(n, 1, 2);
            for (uint32_t sc = 0; sc < n; ++sc)
            {
                const double amplitude = allocated(int(sc) - centre, u) ?
                                             (sc % 2 ? 2.0 : 1.0) : 0.0;
                h.Set(sc, 0, u, {std::sqrt(2.0) * amplitude, 0});
            }
            snapshot.users.push_back({u + 1, aps.Get(0)->GetId(), stas.Get(u)->GetId(),
                                       MultiUserChannelDirection::DOWNLINK, NanoSeconds(0),
                                       {1, 1, .5, .5, "dipole", "V"}, frequencies, 0, h});
        }
        DlMuMimoPrecodingMatrix precoder(
            MimoPrecodingMatrix::FrequencyFlat(n, 2, 2, {{1, 0}, {0, 0}, {0, 0}, {1, 0}}),
            {{1, 0}, {2, 0}});
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetReceiverNoiseFigureDb(7);
        adapter->SetEesmBeta(0, 2);
        MimoChannelMatrix fallback(n, 1, 1);
        for (uint32_t sc = 0; sc < n; ++sc) fallback.Set(sc, 0, 0, {1, 0});
        adapter->SetFixedChannelMatrix(fallback);
        adapter->SetDlMuMimoState(snapshot, precoder);
        for (uint32_t invalidKind = 0; invalidKind < 4; ++invalidKind)
        {
            auto invalid = snapshot;
            switch (invalidKind)
            {
            case 0: invalid.users[1].txNodeId = aps.Get(1)->GetId(); break;
            case 1: invalid.users[1].rxNodeId = invalid.users[0].rxNodeId; break;
            case 2: invalid.users[1].rxNodeId = invalid.users[1].txNodeId; break;
            case 3: invalid.users[1].widebandLossDb = std::numeric_limits<double>::quiet_NaN(); break;
            }
            bool rejected = false;
            try
            {
                adapter->SetDlMuMimoState(invalid, precoder);
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid installed endpoint/loss was accepted");
            NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(), true, "Rejection cleared valid state");
        }
        auto params = Create<WifiSpectrumSignalParameters>();
        params->ppdu = ppdu;
        params->duration = MicroSeconds(100);
        params->txPhy = apPhy->GetCurrentInterface();
        params->psd = psd;
        const double bandwidth = type == HeRu::RU_242_TONE ? 20e6 :
                                 type == HeRu::RU_106_TONE ? 8e6 : 2e6;
        const double noise = 1.3803e-23 * 290 * bandwidth * std::pow(10.0, .7);
        for (uint32_t pass = 0; pass < 2; ++pass)
        {
            if (pass == 1)
            {
                // Perturb all unallocated bins including DC/guards and the other RU.
                for (uint32_t u = 0; u < 2; ++u)
                    for (uint32_t sc = 0; sc < n; ++sc)
                        if (!allocated(int(sc) - centre, u))
                            snapshot.users[u].channel.Set(sc, 0, u, {50, 0});
            }
            if (pass == 1)
            {
                adapter->SetDlMuMimoState(snapshot, precoder);
            }
            // Pass zero also verifies that rejected replacements preserved the
            // previously installed state's actual receive behavior.
            for (uint32_t u = 0; u < 2; ++u)
            {
                int lo = type == HeRu::RU_26_TONE ? -16 :
                         type == HeRu::RU_106_TONE && u == 1 ? 17 : -122;
                int hi = type == HeRu::RU_26_TONE ? 16 :
                         type == HeRu::RU_106_TONE && u == 0 ? -17 : 122;
                auto integrate = [&](const SpectrumValue& value) {
                    double sum = 0;
                    for (int tone = lo; tone <= hi; ++tone)
                        sum += value.ValuesAt(centre + tone) * 78125;
                    return sum;
                };
                const double rho = integrate(*psd) / noise;
                double exponentialSum = 0;
                uint32_t count = 0;
                for (int tone = lo; tone <= hi; ++tone)
                {
                    if (!allocated(tone, u)) continue;
                    const double gain = (centre + tone) % 2 ? 4.0 : 1.0;
                    const double sinr = rho * gain * (type == HeRu::RU_106_TONE ? 2 : 1);
                    exponentialSum += std::exp(-sinr / 2);
                    ++count;
                }
                const double expected = -2 * std::log(exponentialSum / count);
                const auto output = adapter->CalcRxPowerSpectralDensity(
                    params, aps.Get(0)->GetObject<MobilityModel>(),
                    stas.Get(u)->GetObject<MobilityModel>());
                NS_TEST_ASSERT_MSG_EQ_TOL(integrate(*output) / noise, expected, 1e-4,
                                         "RU allocation or null-tone exclusion changed effective SINR");
                if (type == HeRu::RU_106_TONE)
                {
                    const int otherRuTone = u == 0 ? 20 : -20;
                    NS_TEST_ASSERT_MSG_EQ_TOL(output->ValuesAt(centre + otherRuTone), 0.0, 1e-30,
                                             "Other RU ignored its orthogonal transmit precoder");
                }
            }
        }
        // The non-HE waveform has a four-times coarser frequency grid. Its
        // physical gain must follow frequency centres, regardless of payload beta.
        auto commonPsd = WifiSpectrumValueHelper::CreateDuplicated20MhzTxPowerSpectralDensity(
            5180, 20, 1e-12, apPhy->GetGuardBandwidth(20));
        params->ppdu = Create<HePpdu>(psdus, tx, apPhy->GetOperatingChannel(),
                                      MicroSeconds(100), 2, HePpdu::PSD_NON_HE_PORTION);
        params->psd = commonPsd;
        adapter->SetEesmBeta(0, 40);
        const auto commonOutput = adapter->CalcRxPowerSpectralDensity(
            params, aps.Get(0)->GetObject<MobilityModel>(), stas.Get(0)->GetObject<MobilityModel>());
        uint32_t bin = 0;
        for (auto band = commonPsd->GetSpectrumModel()->Begin();
             bin + 1 < commonPsd->GetValuesN(); ++band, ++bin)
        {
            const int tone = std::lround((band->fc - 5180e6) / 78125.0);
            // Every occupied coarse-grid centre hits an even HE tone, with gain 1.
            // The perturbed outside-RU coefficient is 50 on one of two TX branches.
            const double gain = allocated(tone, 0) ? 1.0 : 1250.0;
            NS_TEST_ASSERT_MSG_EQ_TOL(commonOutput->ValuesAt(bin),
                                     commonPsd->ValuesAt(bin) * gain, 1e-25,
                                     "Common preamble used payload beta or the wrong CFR frequency");
        }
        params->ppdu = ppdu;
        params->psd = psd;
        // Normal overhearing and another AP must preserve physical energy.
        for (const auto& receiver : {stas.Get(0), stas.Get(2)})
        {
            auto txNode = receiver == stas.Get(0) ? aps.Get(1) : aps.Get(0);
            auto txDevice = DynamicCast<WifiNetDevice>(apDevices.Get(txNode == aps.Get(0) ? 0 : 1));
            params->txPhy = DynamicCast<SpectrumWifiPhy>(txDevice->GetPhy())->GetCurrentInterface();
            const auto output = adapter->CalcRxPowerSpectralDensity(
                params, txNode->GetObject<MobilityModel>(), receiver->GetObject<MobilityModel>());
            NS_TEST_ASSERT_MSG_EQ_TOL(Integral(*output), Integral(*psd), 1e-24,
                                     "Bystander/unrelated AP changed physical propagation");
            NS_TEST_ASSERT_MSG_EQ(adapter->HasDlMuMimoState(), true, "Overhearing cleared MU state");
        }
    }
};
class HeDlMuSpectrumRuSuite : public TestSuite
{
  public:
    HeDlMuSpectrumRuSuite() : TestSuite("he-dl-mu-spectrum-ru", UNIT)
    {
        AddTestCase(new HeDlMuSpectrumRuTest, TestCase::QUICK);
    }
} g_heDlMuSpectrumRuSuite;
} // namespace
