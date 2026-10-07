/* SPDX-License-Identifier: GPL-2.0-only
 * Live R4 benchmark: production IPC/cache plus direct HE-SU SpectrumWifiPhy.
 * Beta=2 is an explicit diagnostic parameter, not a calibrated PER model.
 */
#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/wifi-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-propagation-cache.h"
#include "ns3/sionna-propagation-delay-model.h"
#include "ns3/sionna-propagation-loss-model.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace ns3;
namespace
{
using Clock = std::chrono::steady_clock;
uint64_t Elapsed(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Memory
{
    uint64_t rss{0}, hwm{0};
};
Memory ReadMemory()
{
    Memory result;
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);)
    {
        std::istringstream values(line);
        std::string key;
        uint64_t kib;
        if (values >> key >> kib)
        {
            if (key == "VmRSS:") result.rss = kib * 1024;
            if (key == "VmHWM:") result.hwm = kib * 1024;
        }
    }
    Require(result.rss > 0 && result.hwm > 0, "Cannot read native Linux RSS/HWM");
    return result;
}
const char* RESOURCE_HEADER =
    "rss_bytes,hwm_bytes,cache_links,cache_snapshots,cache_payload_bytes,cache_capacity_bytes,"
    "cache_peak_links,cache_peak_snapshots,cache_peak_payload_bytes,cache_peak_capacity_bytes,"
    "pool_capacity_bytes,pool_peak_capacity_bytes";
struct Observations
{
    Ptr<SionnaPropagationCache> cache;
    bool cacheEnabled{true};
    uint32_t antennas{1}, receivers{1}, epoch{0}, packet{0};
    uint64_t traceCount{0}, adapterCount{0}, scalarCount{0};
    uint64_t rxOk{0}, rxError{0}, rxDrop{0}, peakRss{0}, peakHwm{0};
    std::set<std::pair<uint64_t, uint32_t>> successful, failed;
    std::ofstream sends, propagation, channels, receptions;
    uint64_t Index() const { return uint64_t(epoch) * 4 + packet; }
    void Resources(std::ostream& stream)
    {
        const auto memory = ReadMemory();
        peakRss = std::max(peakRss, memory.rss);
        peakHwm = std::max(peakHwm, memory.hwm);
        const auto storage = cache->GetStorageStatistics();
        const auto pool = cache->GetPoolStatistics();
        Require(cacheEnabled || (storage.links == 0 && storage.snapshots == 0 &&
                                   storage.payloadBytes == 0 && storage.capacityBytes == 0),
                "Disabled cache retained channel storage");
        stream << memory.rss << ',' << memory.hwm << ',' << storage.links << ','
               << storage.snapshots << ',' << storage.payloadBytes << ',' << storage.capacityBytes
               << ',' << storage.peakLinks << ',' << storage.peakSnapshots << ','
               << storage.peakPayloadBytes << ',' << storage.peakCapacityBytes << ','
               << pool.capacityBytes << ',' << pool.peakCapacityBytes;
    }
    void Mimo(uint64_t uid, uint32_t tx, uint32_t rx, double sinr, double per, uint32_t rank)
    {
        Require(tx == 0 && rx >= 1 && rx <= receivers, "Unexpected MIMO trace endpoint");
        Require(std::isfinite(sinr) && sinr >= 0 && std::isfinite(per) && per >= 0 && per <= 1 &&
                    rank <= antennas, "Nonfinite or invalid MIMO metrics");
        ++traceCount;
        channels << epoch << ',' << packet << ',' << Simulator::Now().GetNanoSeconds() << ','
                 << rx << ',' << uid << ",mimo_rx," << sinr << ',' << per << ',' << rank << '\n';
    }
    void Reception(uint32_t rx, const char* outcome, double sinr = -1, int reason = -1)
    {
        receptions << epoch << ',' << packet << ',' << Simulator::Now().GetNanoSeconds()
                   << ',' << rx << ',' << outcome << ',';
        if (sinr >= 0) receptions << sinr;
        receptions << ',';
        if (reason >= 0) receptions << reason;
        receptions << '\n';
    }
};

// The channel schedules spectral propagation after Send(). Forward to the
// unmodified adapter and time that boundary separately from WifiPhy::Send().
class TimedSionnaLoss : public SpectrumPropagationLossModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::LiveBenchmarkTimedSionnaLoss")
            .SetParent<SpectrumPropagationLossModel>().AddConstructor<TimedSionnaLoss>();
        return tid;
    }
    Ptr<SionnaSpectrumPropagationLossModel> adapter;
    Observations* observations{nullptr};
  private:
    Ptr<SpectrumValue> DoCalcRxPowerSpectralDensity(
        Ptr<const SpectrumSignalParameters> params, Ptr<const MobilityModel> tx,
        Ptr<const MobilityModel> rx) const override
    {
        const auto start = Clock::now();
        const auto result = adapter->CalcRxPowerSpectralDensity(params, tx, rx);
        const uint64_t ns = Elapsed(start);
        const uint32_t receiver = rx->GetObject<Node>()->GetId();
        ++observations->adapterCount;
        observations->propagation << observations->epoch << ',' << observations->packet << ','
            << Simulator::Now().GetNanoSeconds() << ',' << receiver << ',' << ns << ',';
        observations->Resources(observations->propagation);
        observations->propagation << '\n';
        return result;
    }
};
struct Receiver
{
    Observations* observations;
    uint32_t id;
    void Ok(Ptr<const WifiPsdu>, RxSignalInfo info, WifiTxVector tx, std::vector<bool>)
    {
        Require(std::isfinite(info.snr) && info.snr >= 0 && std::isfinite(info.rssi),
                "Nonfinite scalar PHY reception metrics");
        Require(tx.GetNss() == observations->antennas, "Received stream dimension mismatch");
        ++observations->rxOk;
        observations->successful.emplace(observations->Index(), id);
        observations->Reception(id, "rx_ok", info.snr);
        if (observations->antennas == 1)
        {
            ++observations->scalarCount;
            observations->channels << observations->epoch << ',' << observations->packet << ','
                << Simulator::Now().GetNanoSeconds() << ',' << id
                << ",,scalar_rx_ok," << info.snr << ",,1\n";
        }
    }
    void Error(Ptr<const WifiPsdu>)
    {
        ++observations->rxError;
        observations->failed.emplace(observations->Index(), id);
        observations->Reception(id, "rx_error");
    }
    void Drop(Ptr<const Packet>, WifiPhyRxfailureReason reason)
    {
        ++observations->rxDrop;
        observations->failed.emplace(observations->Index(), id);
        observations->Reception(id, "rx_drop", -1, static_cast<int>(reason));
    }
};
} // namespace

int main(int argc, char* argv[])
{
    const auto startup = Clock::now();
    std::string output, endpoint = "tcp://127.0.0.1:5555";
    uint32_t receivers = 1, antennas = 1, widthMhz = 20, lookaheadDepth = 1;
    uint32_t epochs = 4, packetsPerEpoch = 2, seed = 42;
    uint64_t run = 1;
    bool cacheEnabled = true;
    CommandLine cmd(__FILE__);
    cmd.AddValue("output", "New artifact directory (existing output is preserved)", output);
    cmd.AddValue("endpoint", "Live Sionna REP endpoint", endpoint);
    cmd.AddValue("receivers", "Receiver count: 1 or 4", receivers);
    cmd.AddValue("antennas", "Equal TX/RX ports and streams: 1 or 2", antennas);
    cmd.AddValue("widthMhz", "HE channel width: 20 or 40 MHz", widthMhz);
    cmd.AddValue("cacheEnabled", "Enable production channel cache: 0 or 1", cacheEnabled);
    cmd.AddValue("lookaheadDepth", "CSI depth: 1 or 4", lookaheadDepth);
    cmd.AddValue("epochs", "Coherence epochs: 1 through 100000", epochs);
    cmd.AddValue("packetsPerEpoch", "Packets per epoch: 1 through 4", packetsPerEpoch);
    cmd.AddValue("seed", "Positive ns-3 RNG seed", seed);
    cmd.AddValue("run", "Positive ns-3 RNG run", run);
    cmd.Parse(argc, argv);
    if (output.empty() || endpoint.empty() || (receivers != 1 && receivers != 4) ||
        (antennas != 1 && antennas != 2) || (widthMhz != 20 && widthMhz != 40) ||
        (lookaheadDepth != 1 && lookaheadDepth != 4) || epochs == 0 || epochs > 100000 ||
        packetsPerEpoch == 0 || packetsPerEpoch > 4 || seed == 0 || run == 0)
    {
        std::cerr << "Invalid live benchmark options; output is required\n";
        return 2;
    }
    if (std::filesystem::exists(output))
    {
        std::cerr << "Output already exists: " << output << '\n';
        return 2;
    }
    bool started = false;
    SionnaHelper helper("simple_room/simple_room.xml", endpoint);
    try
    {
        Require(std::filesystem::create_directories(output), "Cannot create output directory");
        Observations observations;
        observations.antennas = antennas;
        observations.receivers = receivers;
        observations.cacheEnabled = cacheEnabled;
        auto open = [&](std::ofstream& stream, const std::string& name) {
            stream.open(std::filesystem::path(output) / name);
            Require(stream.is_open(), "Cannot create " + name);
            stream << std::setprecision(17);
        };
        open(observations.sends, "sends.csv");
        open(observations.propagation, "propagation.csv");
        open(observations.channels, "channels.csv");
        open(observations.receptions, "receptions.csv");
        observations.sends << "epoch,packet,time_ns,sample_time_ns,send_ns,trace_count," << RESOURCE_HEADER << '\n';
        observations.propagation << "epoch,packet,time_ns,receiver,adapter_ns," << RESOURCE_HEADER << '\n';
        observations.channels << "epoch,packet,time_ns,receiver,ppdu_uid,source,sinr_linear,per,rank\n";
        observations.receptions << "epoch,packet,time_ns,receiver,outcome,sinr_linear,drop_reason\n";
        RngSeedManager::SetSeed(seed);
        RngSeedManager::SetRun(run);
        helper.AssignStreams(0);
        helper.SetMode(lookaheadDepth == 1 ? SionnaHelper::MODE_P2MP : SionnaHelper::MODE_P2MP_LAH);
        helper.SetSubMode(receivers * lookaheadDepth);
        helper.Configure(widthMhz == 20 ? 5180 : 5190, widthMhz,
                         widthMhz == 20 ? 256 : 512, 78125, 5);
        auto cache = CreateObject<SionnaPropagationCache>();
        observations.cache = cache;
        cache->SetSionnaHelper(helper);
        cache->SetCaching(cacheEnabled);
        cache->SetOptimize(false);
        auto spectrum = CreateObject<MultiModelSpectrumChannel>();
        auto loss = CreateObject<SionnaPropagationLossModel>();
        loss->SetPropagationCache(cache);
        spectrum->AddPropagationLossModel(loss);
        auto delay = CreateObject<SionnaPropagationDelayModel>();
        delay->SetPropagationCache(cache);
        spectrum->SetPropagationDelayModel(delay);
        auto adapter = CreateObject<SionnaSpectrumPropagationLossModel>();
        adapter->SetPropagationCache(cache);
        auto timed = CreateObject<TimedSionnaLoss>();
        timed->adapter = adapter;
        timed->observations = &observations;
        spectrum->AddSpectrumPropagationLossModel(timed);
        SpectrumWifiPhyHelper txHelper, rxHelper;
        SionnaMimoConfiguration configuration;
        configuration.mode = antennas == 1 ? SionnaMimoMode::SISO :
                                            SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
        configuration.txArray.numColumns = configuration.rxArray.numColumns = antennas;
        configuration.numTxAntennas = configuration.numRxAntennas = configuration.maxNss = antennas;
        configuration.receiverNoiseFigureDb = 7;
        configuration.eesmBetaLinear.emplace(0, 2.0);
        // Isotropic elements avoid a directional null obscuring CPU comparisons.
        configuration.txArray.pattern = configuration.rxArray.pattern = SionnaMimoAntennaPattern::ISOTROPIC;
        SionnaMimoHelper(configuration).ApplyForHeMcs(0, helper, txHelper, rxHelper, adapter);
        for (auto* phy : {&txHelper, &rxHelper})
        {
            phy->SetChannel(spectrum);
            phy->SetErrorRateModel("ns3::TableBasedErrorRateModel");
            phy->Set("ChannelSettings", StringValue("{" + std::to_string(widthMhz == 20 ? 36 : 38) + ", " + std::to_string(widthMhz) + ", BAND_5GHZ, 0}"));
            phy->Set("TxPowerStart", DoubleValue(20));
            phy->Set("TxPowerEnd", DoubleValue(20));
        }
        NodeContainer nodes;
        nodes.Create(receivers + 1);
        for (uint32_t i = 0; i <= receivers; ++i)
        {
            auto mobility = CreateObject<SionnaMobilityModel>();
            mobility->SetPosition(i == 0 ? Vector(3, 2, 2) : Vector(4 + .1 * (i - 1), 3, 1));
            nodes.Get(i)->AggregateObject(mobility);
        }
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        WifiMacHelper mac;
        mac.SetType("ns3::AdhocWifiMac");
        const auto txDevice = DynamicCast<WifiNetDevice>(wifi.Install(txHelper, mac, nodes.Get(0)).Get(0));
        const auto txPhy = DynamicCast<SpectrumWifiPhy>(txDevice->GetPhy());
        txPhy->AssignStreams(2);
        std::vector<Receiver> receiverObservations;
        receiverObservations.reserve(receivers);
        for (uint32_t i = 1; i <= receivers; ++i)
        {
            const auto device = DynamicCast<WifiNetDevice>(wifi.Install(rxHelper, mac, nodes.Get(i)).Get(0));
            const auto phy = DynamicCast<SpectrumWifiPhy>(device->GetPhy());
            Require(phy->GetNumberOfAntennas() == antennas &&
                        phy->GetMaxSupportedRxSpatialStreams() == antennas, "RX PHY dimensions mismatch");
            phy->AssignStreams(2 + i);
            receiverObservations.push_back({&observations, i});
            auto* receiver = &receiverObservations.back();
            phy->SetReceiveOkCallback(MakeCallback(&Receiver::Ok, receiver));
            phy->SetReceiveErrorCallback(MakeCallback(&Receiver::Error, receiver));
            phy->TraceConnectWithoutContext("PhyRxDrop", MakeCallback(&Receiver::Drop, receiver));
        }
        Require(txPhy->GetNumberOfAntennas() == antennas &&
                    txPhy->GetMaxSupportedTxSpatialStreams() == antennas, "TX PHY dimensions mismatch");
        adapter->TraceConnectWithoutContext("MimoRx", MakeCallback(&Observations::Mimo, &observations));
        WifiMacHeader header;
        header.SetType(WIFI_MAC_DATA);
        header.SetAddr1(Mac48Address::GetBroadcast());
        header.SetAddr2(txDevice->GetMac()->GetAddress());
        const auto psdu = Create<WifiPsdu>(Create<Packet>(256), header);
        const WifiTxVector tx(HePhy::GetHeMcs0(), 0, WIFI_PREAMBLE_HE_SU, 800,
                              antennas, antennas, 0, widthMhz, false);
        for (uint32_t e = 0; e < epochs; ++e)
            for (uint32_t p = 0; p < packetsPerEpoch; ++p)
                Simulator::Schedule(MilliSeconds(1 + uint64_t(e) * 6 + p),
                    [&, e, p, txPhy, psdu, tx]() {
                        observations.epoch = e;
                        observations.packet = p;
                        const uint64_t before = observations.traceCount;
                        const int64_t time = Simulator::Now().GetNanoSeconds();
                        const auto start = Clock::now();
                        txPhy->Send({{SU_STA_ID, psdu}}, tx);
                        const uint64_t sendNs = Elapsed(start);
                        Simulator::Schedule(MicroSeconds(900), [&, e, p, time, sendNs, before]() {
                            observations.sends << e << ',' << p << ',' << time << ','
                                << Simulator::Now().GetNanoSeconds() << ',' << sendNs << ','
                                << observations.traceCount - before << ',';
                            observations.Resources(observations.sends);
                            observations.sends << '\n';
                        });
                    });
        Simulator::Stop(MilliSeconds(1 + uint64_t(epochs - 1) * 6 + packetsPerEpoch));
        const auto startHelper = Clock::now();
        helper.Start();
        started = true;
        const auto helperStartNs = Elapsed(startHelper);
        const auto startupNs = Elapsed(startup);
        const auto startRun = Clock::now();
        Simulator::Run();
        const auto runNs = Elapsed(startRun);
        const uint64_t expected = uint64_t(epochs) * packetsPerEpoch * receivers;
        Require(observations.adapterCount == expected, "Missing production adapter callbacks");
        for (const auto& key : observations.successful) observations.failed.erase(key);
        Require(observations.successful.size() + observations.failed.size() == expected,
                "Not every transmitted receiver pair completed a real PHY reception");
        Require(observations.rxOk > 0, "No successful complete PHY reception");
        Require(antennas == 1 ? observations.scalarCount > 0 : observations.traceCount == expected,
                "Missing finite channel metrics from actual PHY path");
        const auto storage = cache->GetStorageStatistics();
        const auto pool = cache->GetPoolStatistics();
        std::ostringstream finalResources;
        observations.Resources(finalResources);
        std::ofstream summary(std::filesystem::path(output) / "summary.json");
        Require(summary.is_open(), "Cannot create summary.json");
        summary << std::setprecision(17)
            << "{\n  \"schema_version\": 1,\n  \"validation\": \"PASS\",\n"
            << "  \"receivers\": " << receivers << ",\n  \"antennas\": " << antennas
            << ",\n  \"width_mhz\": " << widthMhz << ",\n  \"subcarriers\": " << 3 * (widthMhz == 20 ? 256 : 512)
            << ",\n  \"fft_size\": " << (widthMhz == 20 ? 256 : 512)
            << ",\n  \"spectrum_subcarriers\": " << 3 * (widthMhz == 20 ? 256 : 512)
            << ",\n  \"cache_enabled\": " << (cacheEnabled ? "true" : "false")
            << ",\n  \"lookahead_depth\": " << lookaheadDepth << ",\n  \"epochs\": " << epochs
            << ",\n  \"packets_per_epoch\": " << packetsPerEpoch << ",\n  \"seed\": " << seed << ",\n  \"run\": " << run
            << ",\n  \"tx_power_dbm_total\": 20,\n  \"coherence_ms\": 5,\n  \"epoch_spacing_ms\": 6,\n  \"mcs\": 0,\n  \"nss\": " << antennas
            << ",\n  \"eesm_beta_linear\": 2,\n  \"beta_calibrated\": false,\n  \"psdu_bytes\": " << psdu->GetSize()
            << ",\n  \"helper_start_ns\": " << helperStartNs << ",\n  \"startup_ns\": " << startupNs
            << ",\n  \"simulator_run_ns\": " << runNs << ",\n  \"sends\": " << uint64_t(epochs) * packetsPerEpoch
            << ",\n  \"adapter_calls\": " << observations.adapterCount << ",\n  \"mimo_trace_count\": " << observations.traceCount
            << ",\n  \"scalar_metric_count\": " << observations.scalarCount
            << ",\n  \"phy_rx_success_count\": " << observations.successful.size()
            << ",\n  \"phy_rx_failure_count\": " << observations.failed.size()
            << ",\n  \"rx_ok_callbacks\": " << observations.rxOk << ",\n  \"rx_error_callbacks\": " << observations.rxError
            << ",\n  \"rx_drop_callbacks\": " << observations.rxDrop << ",\n  \"cache_hit_ratio\": " << cache->GetStats()
            << ",\n  \"peak_rss_bytes\": " << observations.peakRss << ",\n  \"hwm_bytes\": " << observations.peakHwm
            << ",\n  \"cache_links\": " << storage.links << ",\n  \"cache_snapshots\": " << storage.snapshots
            << ",\n  \"cache_payload_bytes\": " << storage.payloadBytes << ",\n  \"cache_capacity_bytes\": " << storage.capacityBytes
            << ",\n  \"cache_peak_links\": " << storage.peakLinks << ",\n  \"cache_peak_snapshots\": " << storage.peakSnapshots
            << ",\n  \"cache_peak_payload_bytes\": " << storage.peakPayloadBytes
            << ",\n  \"cache_peak_capacity_bytes\": " << storage.peakCapacityBytes
            << ",\n  \"pool_capacity_bytes\": " << pool.capacityBytes << ",\n  \"pool_peak_capacity_bytes\": " << pool.peakCapacityBytes
            << ",\n  \"pool_hits\": " << pool.hits << ",\n  \"pool_misses\": " << pool.misses
            << ",\n  \"pool_allocations\": " << pool.allocations << ",\n  \"pool_bypasses\": " << pool.bypasses << "\n}\n";
        summary.close();
        observations.sends.close(); observations.propagation.close();
        observations.channels.close(); observations.receptions.close();
        Require(!summary.fail() && !observations.sends.fail() && !observations.propagation.fail() &&
                    !observations.channels.fail() && !observations.receptions.fail(), "Artifact write failed");
        Simulator::Destroy();
        helper.Destroy();
        started = false;
        std::cout << "validation=PASS output=" << output << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "live_benchmark_error=" << error.what() << '\n';
        Simulator::Destroy();
        if (started) helper.Destroy();
        return 1;
    }
}
