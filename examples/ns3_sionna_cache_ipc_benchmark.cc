/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Phase 5.2.3: production cache/IPC measurements with frozen Protobuf responses.
 * No RT, link abstraction or private cache instrumentation is introduced.
 */
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mobility-model.h"
#include "ns3/sionna-propagation-cache.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <map>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

using namespace ns3;

namespace
{
using Clock = std::chrono::steady_clock;
using RxInfo = ns3sionna::ChannelStateResponse::ChannelState::RxNodeInfo;
volatile double sink = 0;

void
Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::invalid_argument(message);
    }
}

uint64_t
HeapBytes()
{
    const auto info = mallinfo2();
    // glibc arena-used accounting plus mmap blocks. Freed tcache chunks can
    // remain in uordblks; this is an allocator proxy, not a live-object census.
    return info.uordblks + info.hblkhd;
}

uint64_t
RssBytes(const std::string& label = "VmRSS:")
{
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line))
    {
        if (line.rfind(label, 0) == 0)
        {
            std::istringstream fields(line.substr(label.size()));
            uint64_t kib;
            fields >> kib;
            return kib * 1024;
        }
    }
    throw std::runtime_error("Cannot read Linux resident memory");
}

uint64_t
Bits(double value)
{
    uint64_t bits;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void
ValidateFixture(const ns3sionna::Wrapper& wrapper, uint32_t links)
{
    Require(wrapper.has_channel_state_response(), "Expected a wrapped frozen channel response");
    const auto& response = wrapper.channel_state_response();
    Require(response.csi_size() == 1, "Expected one simultaneous batch");
    const auto& state = response.csi(0);
    Require(state.start_time() == 0 && state.end_time() == 5000000 &&
                state.tx_node().id() == 0 && state.rx_nodes_size() == static_cast<int>(links),
            "Invalid batch identity, validity interval or link count");
    const auto& first = state.rx_nodes(0);
    for (uint32_t link = 0; link < links; ++link)
    {
        const auto& rx = state.rx_nodes(link);
        const uint64_t count = static_cast<uint64_t>(rx.frequencies_size()) *
                               rx.num_rx_antennas() * rx.num_tx_antennas();
        Require(rx.id() == link + 1 && !rx.has_user_identity() &&
                    rx.num_rx_antennas() > 0 && rx.num_tx_antennas() > 0 && count > 0 &&
                    rx.csi_real_size() == rx.csi_imag_size() &&
                    static_cast<uint64_t>(rx.csi_real_size()) == count &&
                    rx.frequencies_size() == first.frequencies_size() &&
                    rx.num_rx_antennas() == first.num_rx_antennas() &&
                    rx.num_tx_antennas() == first.num_tx_antennas() && std::isfinite(rx.wb_loss()),
                "Invalid frozen CFR dimensions, identity or loss");
        for (int i = 0; i < rx.csi_real_size(); ++i)
        {
            Require(std::isfinite(rx.csi_real(i)) && std::isfinite(rx.csi_imag(i)),
                    "Non-finite fixture coefficient");
        }
        for (int sc = 0; sc < rx.frequencies_size(); ++sc)
        {
            Require(rx.frequencies(sc) == (sc - rx.frequencies_size() / 2) * 78125,
                    "Invalid fixture frequency order");
        }
    }
}

struct Sample
{
    std::string operation;
    uint32_t iteration;
    int64_t nanoseconds;
};

template <typename Function>
void
Measure(std::vector<Sample>& samples,
        const std::string& operation,
        uint32_t iterations,
        uint32_t warmup,
        Function function)
{
    for (uint32_t i = 0; i < warmup; ++i)
    {
        function();
    }
    for (uint32_t i = 0; i < iterations; ++i)
    {
        const auto start = Clock::now();
        function();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        samples.push_back({operation, i, elapsed.count()});
    }
}

Ptr<SionnaPropagationCache>
MakeCache(SionnaHelper& helper)
{
    auto cache = CreateObject<SionnaPropagationCache>();
    cache->SetSionnaHelper(helper);
    cache->SetOptimize(false); // Every loss/delay validation must exercise this cache.
    return cache;
}

uint64_t
ValidateCache(Ptr<SionnaPropagationCache> cache,
              const std::vector<Ptr<SionnaMobilityModel>>& radios,
              const ns3sionna::ChannelStateResponse::ChannelState& state)
{
    uint64_t digest = 0;
    uint64_t index = 0;
    for (int link = 0; link < state.rx_nodes_size(); ++link)
    {
        const auto& rx = state.rx_nodes(link);
        auto channel = cache->GetPropagationChannelMatrix(radios[0], radios[link + 1]);
        Require(channel.GetNumSubcarriers() == static_cast<uint32_t>(rx.frequencies_size()) &&
                    channel.GetNumRxAntennas() == rx.num_rx_antennas() &&
                    channel.GetNumTxAntennas() == rx.num_tx_antennas(),
                "Cache changed CFR dimensions");
        for (uint32_t sc = 0; sc < channel.GetNumSubcarriers(); ++sc)
        {
            for (uint32_t r = 0; r < channel.GetNumRxAntennas(); ++r)
            {
                for (uint32_t t = 0; t < channel.GetNumTxAntennas(); ++t, ++index)
                {
                    const uint32_t flat = (sc * channel.GetNumRxAntennas() + r) *
                                          channel.GetNumTxAntennas() + t;
                    const auto value = channel.Get(sc, r, t);
                    Require(Bits(value.real()) == Bits(rx.csi_real(flat)) &&
                                Bits(value.imag()) == Bits(rx.csi_imag(flat)),
                            "Cache changed CFR coefficient or sc/rx/tx layout");
                    // Portable numeric IEEE-754 bit checksum, independently vectorized in NumPy.
                    digest ^= Bits(value.real()) * (2 * index + 1) ^
                              Bits(value.imag()) * (2 * index + 3);
                }
            }
        }
        const auto frequencies = cache->GetPropagationFreq(radios[0], radios[link + 1]);
        Require(std::vector<int>(rx.frequencies().begin(), rx.frequencies().end()) == frequencies,
                "Cache changed frequency order");
        Require(cache->GetPropagationLoss(Ptr<const MobilityModel>(radios[0]),
                                         Ptr<const MobilityModel>(radios[link + 1])) == rx.wb_loss(),
                "Cache changed wideband loss");
        Require(cache->GetPropagationDelay(radios[0], radios[link + 1]).GetNanoSeconds() ==
                    static_cast<int64_t>(rx.delay()),
                "Cache changed delay");
    }
    return digest;
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string input, output, timings, endpoint;
    uint32_t links = 1, iterations = 16, hitIterations = 128, missIterations = 5, warmup = 3;
    CommandLine command(__FILE__);
    command.AddValue("input", "Frozen Wrapper response file", input);
    command.AddValue("output", "JSON measurement output", output);
    command.AddValue("timings", "Per-operation nanosecond CSV", timings);
    command.AddValue("endpoint", "Local IPC frozen-response REP endpoint", endpoint);
    command.AddValue("links", "Number of receivers in the response", links);
    command.AddValue("iterations", "Measured serialization/parse/RTT iterations", iterations);
    command.AddValue("hitIterations", "Measured cache getter hit iterations", hitIterations);
    command.AddValue("missIterations", "Measured cold-cache miss iterations", missIterations);
    command.AddValue("warmup", "Warmup iterations for non-miss measurements", warmup);
    command.Parse(argc, argv);
    try
    {
        Require(links > 0 && links <= 64 && iterations > 0 && hitIterations > 0 &&
                    missIterations > 0 && warmup > 0,
                "Positive counts and at most 64 cached links are required");
        Require(!input.empty() && !output.empty() && !timings.empty() &&
                    input != output && input != timings && output != timings &&
                    endpoint.rfind("ipc://", 0) == 0,
                "Provide distinct input/output/timings paths and a local ipc:// endpoint");
        std::ifstream fixtureStream(input, std::ios::binary);
        Require(fixtureStream.is_open(), "Cannot open frozen response");
        const std::string wire((std::istreambuf_iterator<char>(fixtureStream)), {});
        ns3sionna::Wrapper fixture;
        Require(fixture.ParseFromString(wire), "Cannot parse frozen response");
        ValidateFixture(fixture, links);
        Require(fixture.SerializeAsString() == wire, "Noncanonical frozen response bytes");
        const auto& state = fixture.channel_state_response().csi(0);
        const auto& rx = state.rx_nodes(0);
        RngSeedManager::SetSeed(42);
        RngSeedManager::SetRun(1);
        NodeContainer nodes;
        nodes.Create(links + 1);
        std::vector<Ptr<SionnaMobilityModel>> radios;
        for (uint32_t node = 0; node <= links; ++node)
        {
            auto radio = CreateObject<SionnaMobilityModel>();
            nodes.Get(node)->AggregateObject(radio);
            Require(nodes.Get(node)->GetId() == node, "Unexpected node identity");
            const auto& txPosition = state.tx_node().position();
            if (node == 0)
            {
                radio->SetPosition(Vector(txPosition.x(), txPosition.y(), txPosition.z()));
            }
            else
            {
                const auto& position = state.rx_nodes(node - 1).position();
                radio->SetPosition(Vector(position.x(), position.y(), position.z()));
            }
            radios.push_back(radio);
        }
        SionnaHelper helper("frozen-transport-fixture", endpoint);
        helper.m_zmq_socket.set(zmq::sockopt::linger, 0);
        helper.m_zmq_socket.set(zmq::sockopt::rcvtimeo, 10000);
        helper.m_zmq_socket.set(zmq::sockopt::sndtimeo, 10000);
        ns3sionna::Wrapper request;
        request.mutable_channel_state_request()->set_rx_node(1);
        const std::string requestWire = request.SerializeAsString();
        std::vector<Sample> samples;
        samples.reserve(4 * iterations + hitIterations + missIterations);
        // Real cross-process transport. Request serialization and byte comparison
        // are outside this timer; there is no echo or in-process socket substitute.
        for (uint32_t i = 0; i < warmup + iterations; ++i)
        {
            zmq::message_t reply;
            const auto start = Clock::now();
            Require(helper.m_zmq_socket.send(zmq::buffer(requestWire), zmq::send_flags::none).has_value(),
                    "Frozen request send timed out");
            Require(helper.m_zmq_socket.recv(reply, zmq::recv_flags::none).has_value(),
                    "Frozen response receive timed out");
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
            Require(reply.size() == wire.size() && std::memcmp(reply.data(), wire.data(), wire.size()) == 0,
                    "Actual ZMQ response bytes differ from the frozen reference");
            if (i >= warmup)
            {
                samples.push_back({"ipc_roundtrip", i - warmup, elapsed.count()});
            }
        }
        Measure(samples, "cpp_serialize", iterations, warmup, [&] {
            std::string bytes;
            Require(fixture.SerializeToString(&bytes), "Serialization failed");
            sink = bytes.size();
        });
        Measure(samples, "cpp_parse", iterations, warmup, [&] {
            ns3sionna::Wrapper parsed;
            Require(parsed.ParseFromString(wire), "Deserialization failed");
            sink = parsed.channel_state_response().csi_size();
        });
        Measure(samples, "matrix_decode", iterations, warmup, [&] {
            const auto channel = SionnaPropagationCache::DeserializeChannelMatrix(rx);
            sink = channel.Get(0, 0, 0).real();
        });

        // Measure a live cache and its release with no other benchmark allocations
        // between the heap snapshots. The socket is already warm at the full payload.
        auto memoryCache = MakeCache(helper);
        const uint64_t rssBefore = RssBytes();
        const uint64_t heapBefore = HeapBytes();
        {
            const auto channel = memoryCache->GetPropagationChannelMatrix(radios[0], radios[1]);
            sink = channel.Get(0, 0, 0).real();
        }
        const uint64_t digest = ValidateCache(memoryCache, radios, state);
        // Check logical occupancy independently of allocator/RSS measurements.
        const auto storage = memoryCache->GetStorageStatistics();
        const uint64_t payloadBytes = static_cast<uint64_t>(links) *
            (rx.csi_real_size() * sizeof(std::complex<double>) +
             rx.frequencies_size() * sizeof(int));
        Require(storage.links == links && storage.snapshots == links &&
                    storage.payloadBytes == payloadBytes &&
                    storage.peakLinks == links && storage.peakSnapshots == links &&
                    storage.peakPayloadBytes == payloadBytes &&
                    storage.capacityBytes >= payloadBytes &&
                    storage.peakCapacityBytes == storage.capacityBytes,
                "Cache occupancy does not match the frozen payload");
        const double memoryHitRatio = memoryCache->GetStats();
        Require(std::abs(memoryHitRatio - (4.0 * links) / (4.0 * links + 1)) <= 1e-12,
                "A batched cache fill failed to retain all links");
        const uint64_t rssLive = RssBytes();
        const uint64_t heapLive = HeapBytes();
        memoryCache = nullptr;
        const uint64_t heapReleased = HeapBytes();
        const uint64_t rssReleased = RssBytes();

        auto cache = MakeCache(helper);
        {
            const auto channel = cache->GetPropagationChannelMatrix(radios[0], radios[1]);
            sink = channel.Get(0, 0, 0).real();
        }
        uint32_t hitIndex = 0;
        Measure(samples, "cache_hit", hitIterations, warmup, [&] {
            const auto channel = cache->GetPropagationChannelMatrix(radios[0], radios[1 + hitIndex % links]);
            sink = channel.Get(0, 0, 0).real();
            ++hitIndex;
        });
        // Every miss uses a new cache, avoiding duplicate snapshot accumulation.
        for (uint32_t i = 0; i < missIterations; ++i)
        {
            auto cold = MakeCache(helper);
            const auto start = Clock::now();
            {
                const auto channel = cold->GetPropagationChannelMatrix(radios[0], radios[1]);
                sink = channel.Get(0, 0, 0).real();
            }
            samples.push_back({"cache_miss", i,
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()});
        }
        // Inclusive validity boundary followed by a genuine expiry miss. The REP
        // peer shifts only the timestamp on that response, not the frozen CFR.
        Simulator::Schedule(NanoSeconds(5000000), [&] {
            const auto channel = cache->GetPropagationChannelMatrix(radios[0], radios[1]);
            Require(channel.Get(0, 0, 0) == std::complex<double>(rx.csi_real(0), rx.csi_imag(0)),
                    "Boundary cache hit changed channel");
        });
        Simulator::Schedule(NanoSeconds(5000001), [&] {
            const auto channel = cache->GetPropagationChannelMatrix(radios[0], radios[1]);
            Require(channel.Get(0, 0, 0) == std::complex<double>(rx.csi_real(0), rx.csi_imag(0)),
                    "Expired cache refresh changed channel");
        });
        Simulator::Run();
        const auto refreshed = cache->GetStorageStatistics();
        Require(refreshed.links == links && refreshed.snapshots == links &&
                    refreshed.payloadBytes == payloadBytes && refreshed.peakSnapshots == links,
                "Expiry refresh retained obsolete snapshots or lost inactive links");
        const double hitRatio = cache->GetStats();
        const uint32_t hits = warmup + hitIterations + 1;
        Require(std::abs(hitRatio - static_cast<double>(hits) / (hits + 2)) <= 1e-12,
                "Incorrect hit/miss or inclusive-expiry behavior");
        helper.Destroy();
        Simulator::Destroy();

        struct rusage usage;
        Require(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read peak resident memory");
        const uint64_t coefficients = static_cast<uint64_t>(rx.csi_real_size()) * links;
        std::map<std::string, double> fields{
            {"subcarriers", rx.frequencies_size()}, {"num_tx_antennas", rx.num_tx_antennas()},
            {"num_rx_antennas", rx.num_rx_antennas()}, {"links", links},
            {"response_bytes", wire.size()}, {"request_bytes", requestWire.size()},
            {"complex_coefficients", coefficients}, {"cache_cfr_bytes", coefficients * sizeof(std::complex<double>)},
            {"cache_frequency_bytes", static_cast<uint64_t>(rx.frequencies_size()) * links * sizeof(int)},
            {"cache_hit_count", hits}, {"cache_miss_count", 2}, {"cache_hit_ratio", hitRatio},
            {"memory_validation_hit_ratio", memoryHitRatio},
            {"expected_channel_requests", warmup + iterations + missIterations + 3},
            {"rss_before_fill_bytes", rssBefore}, {"rss_with_cache_bytes", rssLive},
            {"rss_after_release_bytes", rssReleased}, {"peak_rss_bytes", RssBytes("VmHWM:")},
            {"lifetime_peak_rss_bytes", usage.ru_maxrss * 1024.0},
            {"heap_before_fill_bytes", heapBefore}, {"heap_with_cache_bytes", heapLive},
            {"heap_after_release_bytes", heapReleased},
            {"cache_live_heap_delta_bytes", static_cast<double>(heapLive) - heapReleased},
            {"cache_fill_heap_delta_bytes", static_cast<double>(heapLive) - heapBefore},
            {"cpp_protobuf_version", GOOGLE_PROTOBUF_VERSION},
            {"verified_links", links}, {"boundary_hit", 1}, {"expired_miss", 1}};
        int major, minor, patch;
        zmq_version(&major, &minor, &patch);
        std::ofstream result(output);
        Require(result.is_open(), "Cannot write benchmark result");
        result << std::setprecision(17) << "{\n";
        for (const auto& field : fields)
        {
            result << "  \"" << field.first << "\": " << field.second << ",\n";
        }
        result << "  \"coefficient_digest\": \"" << std::hex << std::setw(16) << std::setfill('0')
               << digest << "\",\n  \"cpp_zmq_version\": \"" << std::dec << major << '.' << minor
               << '.' << patch << "\"\n}\n";
        result.close();
        Require(!result.fail(), "Cannot finish benchmark result");
        std::ofstream csv(timings);
        Require(csv.is_open(), "Cannot write timing samples");
        csv << "operation,iteration,nanoseconds\n";
        for (const auto& sample : samples)
        {
            csv << sample.operation << ',' << sample.iteration << ',' << sample.nanoseconds << '\n';
        }
        csv.close();
        Require(!csv.fail(), "Cannot finish timing samples");
    }
    catch (const std::exception& error)
    {
        std::cerr << "benchmark_error=" << error.what() << '\n';
        return 2;
    }
    return 0;
}
