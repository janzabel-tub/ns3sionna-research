/* SPDX-License-Identifier: GPL-2.0-only */
// Thin adapter: evaluate externally supplied fixtures with the production kernels.
// No reference formula or expected numerical result belongs in this program.
#include "ns3/dl-mu-mimo-link-model.h"
#include "ns3/ul-mu-mimo-link-model.h"

#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{
template <typename T>
T Read(std::istream& input)
{
    T value{};
    if (!(input >> value))
    {
        throw std::runtime_error("Truncated or invalid reference fixture");
    }
    return value;
}

std::complex<double> ReadComplex(std::istream& input)
{
    const double real = Read<double>(input);
    const double imag = Read<double>(input);
    return {real, imag};
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
        {
            throw std::runtime_error("Usage: ns3_mumimo_reference_export INPUT.txt OUTPUT.json");
        }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        if (!input || !output)
        {
            throw std::runtime_error("Cannot open input or output");
        }
        const auto count = Read<uint32_t>(input);
        output << std::setprecision(17) << "{\"schema_version\":1,\"cases\":[";
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto id = Read<std::string>(input);
            const auto direction = Read<std::string>(input);
            const auto method = Read<std::string>(input);
            const auto tones = Read<uint32_t>(input);
            const auto tx = Read<uint32_t>(input);
            const auto rx = Read<uint32_t>(input);
            const auto users = Read<uint32_t>(input);
            const auto streams = Read<uint32_t>(input);
            const auto alpha = Read<double>(input);
            if ((direction != "dl" && direction != "ul") || tones == 0 ||
                users == 0 || streams == 0 || tx == 0 || rx == 0)
            {
                throw std::runtime_error("Invalid fixture dimensions or direction");
            }
            std::vector<double> snrs;
            std::vector<uint32_t> groups;
            for (uint32_t user = 0; user < users; ++user)
            {
                snrs.push_back(Read<double>(input));
                groups.push_back(Read<uint32_t>(input));
            }
            std::vector<DlMuMimoStreamMapping> dlMappings;
            std::vector<UlMuMimoStreamMapping> ulMappings;
            for (uint32_t stream = 0; stream < streams; ++stream)
            {
                const auto user = Read<uint32_t>(input);
                const auto localStream = Read<uint32_t>(input);
                dlMappings.push_back({user, localStream});
                ulMappings.push_back({user, localStream});
            }
            const auto dir = direction == "dl" ? MultiUserChannelDirection::DOWNLINK
                                                 : MultiUserChannelDirection::UPLINK;
            MultiUserChannelSnapshot snapshot{NanoSeconds(1234), dir, {}};
            std::vector<int> frequencies;
            for (uint32_t sc = 0; sc < tones; ++sc)
            {
                frequencies.push_back(static_cast<int>(sc));
            }
            for (uint32_t user = 1; user <= users; ++user)
            {
                MimoChannelMatrix channel(tones, rx, tx);
                for (uint32_t sc = 0; sc < tones; ++sc)
                {
                    for (uint32_t r = 0; r < rx; ++r)
                    {
                        for (uint32_t t = 0; t < tx; ++t)
                        {
                            channel.Set(sc, r, t, ReadComplex(input));
                        }
                    }
                }
                snapshot.users.push_back({user,
                                          direction == "dl" ? 100u : user,
                                          direction == "dl" ? user : 100u,
                                          dir,
                                          snapshot.csiTimestamp,
                                          {rx, 1, 0.5, 0.5, "iso", "V"},
                                          frequencies,
                                          0.0,
                                          std::move(channel)});
            }
            std::vector<std::complex<double>> fixed;
            if (direction == "dl" && method == "fixed")
            {
                for (uint64_t j = 0; j < uint64_t(tones) * tx * streams; ++j)
                {
                    fixed.push_back(ReadComplex(input));
                }
            }
            output << (index ? "," : "") << "{\"id\":\"" << id << "\"";
            try
            {
                if (direction == "dl")
                {
                    auto precoder = [&]() {
                        if (method == "fixed")
                        {
                            return DlMuMimoPrecodingMatrix(
                                MimoPrecodingMatrix(tones, tx, streams, fixed), dlMappings);
                        }
                        if (method == "zf")
                        {
                            return DlMuMimoPrecodingMatrix::ZeroForcing(snapshot, dlMappings);
                        }
                        if (method == "rzf")
                        {
                            return DlMuMimoPrecodingMatrix::RegularizedZeroForcing(
                                snapshot, dlMappings, alpha);
                        }
                        throw std::runtime_error("Unknown downlink method");
                    }();
                    std::vector<DlMuMimoUserSnr> budgets;
                    for (uint32_t user = 1; user <= users; ++user)
                    {
                        budgets.push_back({user, snrs[user - 1], groups[user - 1]});
                    }
                    const auto result = DlMuMimoLinkModel::Calculate(snapshot, precoder, budgets);
                    output << ",\"status\":\"ok\",\"precoder\":[";
                    bool first = true;
                    for (const auto z : precoder.GetPrecodingMatrix().GetCoefficients())
                    {
                        output << (first ? "" : ",") << '[' << z.real() << ',' << z.imag() << ']';
                        first = false;
                    }
                    output << "],\"rows\":[";
                    first = true;
                    for (uint32_t user = 1; user <= users; ++user)
                    {
                        const auto& value = result.GetUserById(user);
                        for (uint32_t sc = 0; sc < tones; ++sc)
                        {
                            for (uint32_t stream = 0; stream < value.GetNumStreams(); ++stream)
                            {
                                output << (first ? "" : ",") << '[' << user << ',' << sc << ','
                                       << stream << ',' << value.GetDesiredSignalPower(sc, stream)
                                       << ',' << value.GetInterUserInterferencePower(sc, stream)
                                       << ',' << value.GetSinrLinear(sc, stream) << ']';
                                first = false;
                            }
                        }
                    }
                    output << ']';
                }
                else
                {
                    if (method != "zf" && method != "mmse")
                    {
                        throw std::runtime_error("Unknown uplink method");
                    }
                    std::vector<UlMuMimoUserSnr> budgets;
                    for (uint32_t user = 1; user <= users; ++user)
                    {
                        budgets.push_back({user, snrs[user - 1]});
                    }
                    const auto result = UlMuMimoLinkModel::Calculate(
                        snapshot, ulMappings, budgets,
                        method == "zf" ? UlMuMimoDetector::ZERO_FORCING
                                       : UlMuMimoDetector::LINEAR_MMSE);
                    output << ",\"status\":\"ok\",\"precoder\":[],\"rows\":[";
                    bool first = true;
                    for (uint32_t user = 1; user <= users; ++user)
                    {
                        const auto& value = result.GetUserById(user);
                        for (uint32_t sc = 0; sc < tones; ++sc)
                        {
                            output << (first ? "" : ",") << '[' << user << ',' << sc << ",0,"
                                   << value.GetDesiredSignalPower(sc, 0) << ','
                                   << value.GetInterUserInterferencePower(sc, 0) << ','
                                   << value.GetSinrLinear(sc, 0) << ','
                                   << value.GetNoisePower(sc, 0) << ']';
                            first = false;
                        }
                    }
                    output << ']';
                }
            }
            catch (const std::invalid_argument&)
            {
                output << ",\"status\":\"rejected\",\"precoder\":[],\"rows\":[]";
            }
            output << '}';
        }
        input >> std::ws;
        if (!input.eof())
        {
            throw std::runtime_error("Trailing fixture data");
        }
        output << "]}\n";
        if (!output)
        {
            throw std::runtime_error("Failed to write export");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
