/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/he-effective-sinr-mapping.h"
#include "ns3/he-data-tone-mask.h"
#include "ns3/he-ber-error-rate-model.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/he-dl-mu-mimo-link-abstraction.h"
#include "ns3/he-ul-mu-mimo-link-abstraction.h"
#include "ns3/he-phy.h"
#include "ns3/he-ppdu.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/test.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-phy-operating-channel.h"
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>

namespace ns3
{
namespace
{
WifiTxVector Tx(uint8_t mcs, uint8_t nss = 1)
{
    WifiTxVector tx(HePhy::GetHeMcs(mcs), 0, WIFI_PREAMBLE_HE_SU, 800,
                    nss, nss, 0, 20, false);
    tx.SetLdpc(mcs >= 10);
    return tx;
}
// Independent integration over Gray-labelled PAM decision regions, on each axis.
// Square QAM BER equals the bit average on either independent axis.
double Regions(double gamma, unsigned size)
{
    if (gamma == 0) { return 0.5; }
    const unsigned axis = static_cast<unsigned>(std::sqrt(size));
    const unsigned bits = static_cast<unsigned>(std::log2(axis));
    const double sigma = std::sqrt((size - 1) / (3 * gamma));
    double result = 0;
    const auto cdf = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
    for (unsigned sent = 0; sent < axis; ++sent)
    {
        const double mean = 2.0 * sent - (axis - 1);
        for (unsigned received = 0; received < axis; ++received)
        {
            const double lower = received == 0 ? 0 : cdf((2.0 * received - axis - mean) / sigma);
            const double upper = received == axis - 1 ? 1 : cdf((2.0 * received + 2 - axis - mean) / sigma);
            unsigned difference = (sent ^ (sent >> 1)) ^ (received ^ (received >> 1));
            unsigned errors = 0;
            for (; difference; difference >>= 1) { errors += difference & 1; }
            result += errors * (upper - lower) / (axis * bits);
        }
    }
    return result;
}
class BerMathTest : public TestCase
{
  public:
    BerMathTest() : TestCase("Exact symbol-SINR Gray BER and bounded inverse properties") {}
  private:
    void DoRun() override
    {
        for (double gamma : {0.0, 1e-12, 0.01, 0.1, 1.0, 3.0, 10.0, 100.0, 1000.0})
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::UncodedBer(gamma, 0),
                0.5 * std::erfc(std::sqrt(gamma)), 1e-14, "BPSK symbol SINR");
            NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::UncodedBer(gamma, 1),
                0.5 * std::erfc(std::sqrt(gamma / 2)), 1e-14, "QPSK symbol SINR");
            for (uint8_t mcs : {3, 5, 8, 10})
            {
                NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::UncodedBer(gamma, mcs),
                    Regions(gamma, HeEffectiveSinrMapping::GetConstellationSize(mcs)),
                    2e-14, "QAM independent decision regions");
            }
        }
        for (uint8_t mcs = 0; mcs < 12; ++mcs)
        {
            for (double gamma : {0.0, 1e-15, 1.0, 20.0, 1e6, std::numeric_limits<double>::max()})
            {
                const auto mapped = HeEffectiveSinrMapping::MeanUncodedBer({gamma, gamma}, mcs);
                NS_TEST_ASSERT_MSG_EQ(mapped.effectiveSinrLinear, gamma, "Identical inputs must be exact");
                NS_TEST_ASSERT_MSG_EQ(std::isfinite(mapped.effectiveSinrLinear), true, "Finite endpoint");
            }
            std::vector<double> samples{0, 0.01, 1, 10, 100};
            const auto mapped = HeEffectiveSinrMapping::MeanUncodedBer(samples, mcs);
            NS_TEST_ASSERT_MSG_GT_OR_EQ(mapped.effectiveSinrLinear, 0, "Lower bound");
            NS_TEST_ASSERT_MSG_LT_OR_EQ(mapped.effectiveSinrLinear, 100, "Upper bound");
            NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::UncodedBer(mapped.effectiveSinrLinear, mcs),
                                     mapped.meanUncodedBer, 2e-14, "Inverse BER equality");
            std::reverse(samples.begin(), samples.end());
            NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::MeanUncodedBer(samples, mcs).effectiveSinrLinear,
                                     mapped.effectiveSinrLinear, 2e-13, "Permutation invariance");
            auto duplicate = samples;
            duplicate.insert(duplicate.end(), samples.begin(), samples.end());
            NS_TEST_ASSERT_MSG_EQ_TOL(HeEffectiveSinrMapping::MeanUncodedBer(duplicate, mcs).effectiveSinrLinear,
                                     mapped.effectiveSinrLinear, 2e-13, "Duplication invariance");
            for (auto& value : samples) { value += 1; }
            NS_TEST_ASSERT_MSG_GT(HeEffectiveSinrMapping::MeanUncodedBer(samples, mcs).effectiveSinrLinear,
                                  mapped.effectiveSinrLinear, "Improving bits must improve effective SINR");
            NS_TEST_ASSERT_MSG_EQ(HeEffectiveSinrMapping::MeanUncodedBer({1e200, 1e300}, mcs).effectiveSinrLinear,
                                  1e200, "Underflow must return conservative finite minimum");
        }
        const auto example = HeEffectiveSinrMapping::MeanUncodedBer({10, 1}, 0);
        NS_TEST_ASSERT_MSG_EQ_TOL(10 * std::log10(example.effectiveSinrLinear), 1.89, 0.01,
                                  "Worked BPSK example");
        for (const auto& samples : std::vector<std::vector<double>>{{}, {-1}, {NAN}, {INFINITY}})
        {
            bool rejected = false;
            try { HeEffectiveSinrMapping::MeanUncodedBer(samples, 0); }
            catch (const std::invalid_argument&) { rejected = true; }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid samples accepted");
        }
        bool rejected = false;
        try { HeEffectiveSinrMapping::UncodedBer(1, 12); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid MCS accepted");
    }
};
class ToneMaskTest : public TestCase
{
  public:
    ToneMaskTest() : TestCase("Every HE RU mask matches independent wlanHEOFDMInfo reference") {}
  private:
    void DoRun() override
    {
        const auto path = std::filesystem::path(NS_TEST_SOURCEDIR).parent_path() /
                          "data/he_data_tone_reference.json";
        std::ifstream input(path);
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        google::protobuf::Struct root;
        NS_TEST_ASSERT_MSG_EQ(google::protobuf::util::JsonStringToMessage(json, &root).ok(), true,
                              "Missing MATLAB tone reference");
        unsigned checked = 0;
        for (const auto& value : root.fields().at("rows").list_value().values())
        {
            const auto& row = value.struct_value();
            const unsigned width = row.fields().at("width").number_value();
            const unsigned size = row.fields().at("size").number_value();
            const unsigned index = row.fields().at("index").number_value();
            const HeRu::RuType type = size == 26 ? HeRu::RU_26_TONE : size == 52 ? HeRu::RU_52_TONE :
                size == 106 ? HeRu::RU_106_TONE : size == 242 ? HeRu::RU_242_TONE :
                size == 484 ? HeRu::RU_484_TONE : size == 996 ? HeRu::RU_996_TONE : HeRu::RU_2x996_TONE;
            const auto tones = HeDataToneMask::GetDataTones(width, type, index);
            const auto& data = row.fields().at("data").list_value().values();
            NS_TEST_ASSERT_MSG_EQ(tones.size(), static_cast<std::size_t>(data.size()), "HE data count");
            for (unsigned i = 0; i < tones.size(); ++i)
            { NS_TEST_ASSERT_MSG_EQ(tones[i], data[i].number_value(), "HE data tone mismatch"); }
            for (const auto& pilot : row.fields().at("pilots").list_value().values())
            { NS_TEST_ASSERT_MSG_EQ(std::binary_search(tones.begin(), tones.end(), int(pilot.number_value())),
                                   false, "Pilot entered data mask"); }
            ++checked;
        }
        NS_TEST_ASSERT_MSG_EQ(checked, 254, "Not all physical RU configurations covered");
    }
};
class BerAwgnTest : public TestCase
{
  public:
    BerAwgnTest() : TestCase("Uniform HE SINR retains ns-3 coded AWGN parity without betas") {}
  private:
    void DoRun() override
    {
        auto model = CreateObject<HeSuMimoLinkAbstraction>();
        auto base = CreateObject<TableBasedErrorRateModel>();
        model->SetErrorRateModel(base);
        const MimoChannelMatrix flat(4, 1, 1, {{1,0},{1,0},{1,0},{1,0}});
        for (uint8_t mcs = 0; mcs < 12; ++mcs)
        {
            for (bool ldpc : {false, true})
            {
                if (mcs >= 10 && !ldpc) { continue; }
                auto tx = Tx(mcs); tx.SetLdpc(ldpc);
                for (double snr : {0.1, 1.0, 10.0, 100.0, 1000.0})
                {
                    const auto budget = WifiLinkBudget::Calculate(0, 0, 20e6, 290, 0);
                    const auto scaled = WifiLinkBudget::Calculate(
                        10 * std::log10(snr * budget.GetReceiverNoisePowerW()) + 30, 0, 20e6, 290, 0);
                    for (uint32_t bytes : {32, 1200})
                    {
                        const auto result = model->Calculate(flat, scaled, tx, bytes);
                        const auto effective = result.GetEffectiveSinrLinear();
                        NS_TEST_ASSERT_MSG_EQ_TOL(effective, snr, snr * 1e-12, "AWGN identity");
                        NS_TEST_ASSERT_MSG_EQ(result.GetPacketSuccessRate(),
                            base->GetChunkSuccessRate(tx.GetMode(), tx, effective, bytes * 8ULL, 1),
                            "Uniform SINR must use the existing coded AWGN curve");
                    }
                }
            }
        }
        // Beta alone leaves the selected model unchanged; replacing config retains selector.
        model->SetEesmBeta(0, 100);
        NS_TEST_ASSERT_MSG_EQ_TOL(model->CalculateEffectiveSinr({10.0,1.0},0),
            HeEffectiveSinrMapping::MeanUncodedBer({10,1},0).effectiveSinrLinear, 1e-14, "Beta switched mapping");
        const auto zeroTx = Tx(0);
        model->SetAwgnSnrOffsetDb({0,false,32,1,1,20,800},-10);
        NS_TEST_ASSERT_MSG_EQ(model->GetErrorModelSinrLinear(0,zeroTx,32,1,SU_STA_ID,3),3,
                              "General mode applied an EESM calibration offset");
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel::EESM);
        adapter->ConfigureEesmBetas({{0,2}});
        adapter->ConfigureAwgnSnrOffsetsDb({});
        NS_TEST_ASSERT_MSG_EQ(int(adapter->GetEffectiveSinrModel()), int(SionnaMimoEffectiveSinrModel::EESM),
                              "Replacement lost mapping selector");
        SionnaMimoConfiguration configuration;
        configuration.mode = SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        configuration.numTxAntennas = configuration.numRxAntennas = configuration.maxNss = 2;
        configuration.txArray.numColumns = configuration.rxArray.numColumns = 2;
        SionnaMimoHelper::ValidateForHeMcs(configuration, 11);
        configuration.effectiveSinrModel = SionnaMimoEffectiveSinrModel::LEGACY_SISO;
        bool rejected = false;
        try { SionnaMimoHelper::Validate(configuration); }
        catch (const std::invalid_argument&) { rejected = true; }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Legacy MIMO accepted");
    }
};
class BerSpatialTest : public TestCase
{
  public:
    BerSpatialTest() : TestCase("Beta-free SU detectors, precoding and independent DL and UL MU mappings") {}
  private:
    MultiUserChannel User(uint32_t id, const MimoChannelMatrix& h, MultiUserChannelDirection direction)
    {
        return {id, direction == MultiUserChannelDirection::DOWNLINK ? 100u : id,
                direction == MultiUserChannelDirection::DOWNLINK ? id : 100u,
                direction, NanoSeconds(1),
                {h.GetNumRxAntennas(),h.GetNumTxAntennas(),0.5,0.5,"dipole","V"}, {0,1}, 0, h};
    }
    void DoRun() override
    {
        const auto noise = WifiLinkBudget::Calculate(0,0,20e6,290,0).GetReceiverNoisePowerW();
        const auto budget = WifiLinkBudget::Calculate(10*std::log10(20*noise)+30,0,20e6,290,0);
        auto su = CreateObject<HeSuMimoLinkAbstraction>();
        MimoChannelMatrix balanced(2,2,2), unequal(2,2,2), deficient(2,2,2);
        for (unsigned k = 0; k < 2; ++k)
        {
            balanced.Set(k,0,0,{std::sqrt(2.0),0}); balanced.Set(k,1,1,{std::sqrt(2.0),0});
            unequal.Set(k,0,0,{std::sqrt(2.0),0}); unequal.Set(k,1,1,{0.1,0});
            deficient.Set(k,0,0,{1,0}); deficient.Set(k,0,1,{1,0});
            deficient.Set(k,1,0,{1,0}); deficient.Set(k,1,1,{1,0});
        }
        const auto identity = MimoPrecodingMatrix::FrequencyFlat(2,2,2,{{1,0},{0,0},{0,0},{1,0}});
        for (const auto& h : {balanced, unequal, deficient})
        {
            const auto result = su->Calculate(h,budget,Tx(3,2),1200);
            std::vector<double> values;
            for (unsigned k = 0; k < 2; ++k)
            { for (unsigned stream = 0; stream < 2; ++stream)
              { values.push_back(result.GetMimoResult().GetSinrLinear(k,stream)); } }
            NS_TEST_ASSERT_MSG_EQ(result.GetEffectiveSinrLinear(),
                HeEffectiveSinrMapping::MeanUncodedBer(values,3).effectiveSinrLinear,"SU uses shared mapper");
            NS_TEST_ASSERT_MSG_EQ_TOL(su->Calculate(h,identity,budget,Tx(3,2),1200).GetEffectiveSinrLinear(),
                                     result.GetEffectiveSinrLinear(),1e-12,"Fixed identity precoding");
        }
        NS_TEST_ASSERT_MSG_GT(su->Calculate(balanced,budget,Tx(3,2),1200).GetEffectiveSinrLinear(),
                              su->Calculate(unequal,budget,Tx(3,2),1200).GetEffectiveSinrLinear(),"Weak stream ignored");
        NS_TEST_ASSERT_MSG_EQ(su->Calculate(deficient,budget,Tx(3,2),1200).GetMimoResult().GetMinimumChannelRank(),
                              1,"Rank deficiency disappeared");
        const auto svd = MimoPrecodingMatrix::SingularValueDecomposition(unequal,2);
        NS_TEST_ASSERT_MSG_EQ_TOL(su->Calculate(unequal,svd.GetPrecodingMatrix(),budget,Tx(3,2),1200).GetEffectiveSinrLinear(),
            su->Calculate(unequal,identity,budget,Tx(3,2),1200).GetEffectiveSinrLinear(),1e-12,"SVD added mapping penalty");
        MimoChannelMatrix miso(2,1,2,{{1,0},{1,0},{0.5,0},{0.5,0}});
        const auto mrt = MimoPrecodingMatrix::MaximumRatioTransmission(miso);
        const auto beam = su->Calculate(miso,mrt,budget,Tx(0),1200);
        NS_TEST_ASSERT_MSG_EQ_TOL(beam.GetEffectiveSinrLinear(),
            HeEffectiveSinrMapping::MeanUncodedBer({40,10},0).effectiveSinrLinear,1e-10,"MRT receiver gain");
        auto dl = CreateObject<HeDlMuMimoLinkAbstraction>();
        auto ul = CreateObject<HeUlMuMimoLinkAbstraction>();
        const DlMuMimoPrecodingMatrix precoder(identity,{{3,0},{9,0}});
        for (double cross : {0.0, 0.5, 1.0})
        {
            MimoChannelMatrix h3(2,1,2,{{1,0},{cross,0},{0.1,0},{cross,0}});
            MimoChannelMatrix h9(2,1,2,{{cross,0},{1,0},{cross,0},{2,0}});
            const MultiUserChannelSnapshot down{NanoSeconds(1),MultiUserChannelDirection::DOWNLINK,
                {User(3,h3,MultiUserChannelDirection::DOWNLINK),User(9,h9,MultiUserChannelDirection::DOWNLINK)}};
            auto result = dl->Calculate(down,precoder,{{3,budget,Tx(0),1200},{9,budget,Tx(3),400}});
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto& user = result.GetUser(i); const auto& detector = user.GetMimoResult();
                const auto mcs = i == 0 ? 0 : 3;
                NS_TEST_ASSERT_MSG_EQ(user.GetEffectiveSinrLinear(),
                    HeEffectiveSinrMapping::MeanUncodedBer({detector.GetSinrLinear(0,0),detector.GetSinrLinear(1,0)},mcs).effectiveSinrLinear,
                    "DL MU users did not map independently");
            }
            const auto selected = dl->Calculate(down,precoder,
                {{3,budget,Tx(0),1200,SU_STA_ID,0,{0}},{9,budget,Tx(3),400,SU_STA_ID,0,{1}}});
            NS_TEST_ASSERT_MSG_EQ(selected.GetUserById(3).GetEffectiveSinrLinear(),
                result.GetUserById(3).GetMimoResult().GetSinrLinear(0,0),"Selected user tones ignored");
            const auto changed = dl->Calculate(down,precoder,{{3,budget,Tx(0),1200},{9,budget,Tx(11),400}});
            NS_TEST_ASSERT_MSG_EQ(changed.GetUserById(3).GetEffectiveSinrLinear(),
                result.GetUserById(3).GetEffectiveSinrLinear(),"Another user's modulation changed own BER");
            MimoChannelMatrix g3(2,2,1,{{1,0},{cross,0},{0.1,0},{cross,0}});
            MimoChannelMatrix g9(2,2,1,{{cross,0},{1,0},{cross,0},{2,0}});
            const MultiUserChannelSnapshot up{NanoSeconds(1),MultiUserChannelDirection::UPLINK,
                {User(3,g3,MultiUserChannelDirection::UPLINK),User(9,g9,MultiUserChannelDirection::UPLINK)}};
            const auto received = ul->Calculate(up,UlMuMimoDetector::LINEAR_MMSE,
                {{3,budget,Tx(0),1200},{9,budget,Tx(3),400}});
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto& user = received.GetUser(i); const auto& detector = user.GetMimoResult();
                NS_TEST_ASSERT_MSG_EQ(user.GetEffectiveSinrLinear(),
                    HeEffectiveSinrMapping::MeanUncodedBer({detector.GetSinrLinear(0,0),detector.GetSinrLinear(1,0)},i == 0 ? 0 : 3).effectiveSinrLinear,
                    "UL MU shared mapper");
            }
        }
    }
};
class BerWrapperTest : public TestCase
{
  public:
    BerWrapperTest() : TestCase("Receiver-local BER payload chunks, interference scaling and expiry") {}
  private:
    void DoRun() override
    {
        const auto tx = Tx(3);
        WifiMacHeader header; header.SetType(WIFI_MAC_DATA);
        const auto psdu = Create<WifiPsdu>(Create<Packet>(400), header);
        WifiPhyOperatingChannel operating;
        operating.SetDefault(20, WIFI_STANDARD_80211ax, WIFI_PHY_BAND_5GHZ);
        const auto ppdu = Create<HePpdu>(psdu, tx, operating, MicroSeconds(100), 77);
        auto base = CreateObject<TableBasedErrorRateModel>();
        auto wrapper = CreateObject<HeBerErrorRateModel>(); wrapper->Configure(base);
        wrapper->SetReception(77, {0.1, 2, 10, 30}, 5, MicroSeconds(100));
        auto other = CreateObject<HeBerErrorRateModel>(); other->Configure(base);
        other->SetReception(77, {40,40}, 5, MicroSeconds(100));
        for (double scalar : {0.0, 0.5, 2.5, 5.0})
        {
            const auto effective = HeEffectiveSinrMapping::MeanUncodedBer(
                {0.1*scalar/5,2*scalar/5,10*scalar/5,30*scalar/5},3).effectiveSinrLinear;
            for (uint64_t bits : {117, 1000, 3424})
            {
                NS_TEST_ASSERT_MSG_EQ(wrapper->GetPayloadChunkSuccessRate(tx.GetMode(), tx, scalar,
                    bits, 1, SU_STA_ID, ppdu, 0.25),
                    base->GetChunkSuccessRate(tx.GetMode(), tx, effective, bits, 1), "Native chunk bits/FEC");
            }
            NS_TEST_ASSERT_MSG_EQ_TOL(other->GetPayloadChunkSuccessRate(tx.GetMode(), tx, scalar,
                117, 1, SU_STA_ID, ppdu, 1),
                base->GetChunkSuccessRate(tx.GetMode(), tx, 8*scalar, 117, 1),1e-12,"Receiver isolation");
        }
        NS_TEST_ASSERT_MSG_EQ(wrapper->GetChunkSuccessRate(HePhy::GetOfdmRate6Mbps(), tx, 5,
            48,1,WIFI_PPDU_FIELD_NON_HT_HEADER),
            base->GetChunkSuccessRate(HePhy::GetOfdmRate6Mbps(),tx,5,48,1,WIFI_PPDU_FIELD_NON_HT_HEADER),
            "Header must keep physical scalar SNR");
        NS_TEST_ASSERT_MSG_EQ(wrapper->GetReceptionCount(),1,"Missing receiver snapshot");
        Simulator::Stop(MicroSeconds(101)); Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(wrapper->GetReceptionCount(),0,"Snapshot survived PPDU end");
        NS_TEST_ASSERT_MSG_EQ(wrapper->GetPayloadChunkSuccessRate(tx.GetMode(),tx,5,117,1,
            SU_STA_ID,ppdu,1),base->GetChunkSuccessRate(tx.GetMode(),tx,5,117,1),"Expired UID applied mapping");
        Simulator::Destroy();
    }
};
}
class HeEffectiveSinrSuite : public TestSuite
{
  public:
    HeEffectiveSinrSuite() : TestSuite("he-effective-sinr-mapping", UNIT)
    {
        AddTestCase(new BerMathTest, TestCase::QUICK);
        AddTestCase(new ToneMaskTest, TestCase::QUICK);
        AddTestCase(new BerAwgnTest, TestCase::QUICK);
        AddTestCase(new BerSpatialTest, TestCase::QUICK);
        AddTestCase(new BerWrapperTest, TestCase::QUICK);
    }
};
static HeEffectiveSinrSuite g_heEffectiveSinrSuite;
}
