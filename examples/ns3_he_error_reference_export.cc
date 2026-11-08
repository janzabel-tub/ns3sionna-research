/* SPDX-License-Identifier: GPL-2.0-only */
// Evaluate external stream/tone SINRs through the production HE abstractions.
#include "ns3/he-effective-sinr-mapping.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/he-dl-mu-mimo-link-abstraction.h"
#include "ns3/he-phy.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;
namespace
{
template <typename T> T Read(std::istream& in)
{
    T value{};
    if (!(in >> value)) { throw std::runtime_error("Invalid error-reference fixture"); }
    return value;
}
WifiLinkBudgetResult Budget(double snr)
{
    constexpr double k = 1.3803e-23;
    return WifiLinkBudget::Calculate(10*std::log10(snr*k)+30,0,1,1,0);
}
WifiTxVector Tx(uint8_t mcs, uint8_t streams, bool ldpc)
{
    WifiTxVector tx(HePhy::GetHeMcs(mcs),0,WIFI_PREAMBLE_HE_SU,800,
                    streams,streams,0,20,false);
    tx.SetLdpc(ldpc);
    return tx;
}
}
int main(int argc,char** argv)
{
    try
    {
        if (argc != 3) { throw std::runtime_error("Usage: exporter INPUT OUTPUT"); }
        std::ifstream in(argv[1]); std::ofstream out(argv[2]);
        if (!in || !out) { throw std::runtime_error("Cannot open fixture/output"); }
        out << std::setprecision(17) << "{\"schema_version\":1,\"cases\":[";
        const auto count = Read<unsigned>(in);
        for (unsigned i=0;i<count;++i)
        {
            const auto id=Read<std::string>(in);
            const auto mcs=Read<unsigned>(in), streams=Read<unsigned>(in);
            const auto tones=Read<unsigned>(in), bytes=Read<unsigned>(in);
            const bool ldpc=Read<unsigned>(in);
            const auto selectionCount=Read<unsigned>(in);
            if (mcs>11 || streams==0 || streams>8 || tones==0 || bytes==0)
            { throw std::runtime_error("Invalid PHY dimensions"); }
            std::vector<uint32_t> selected;
            for (unsigned j=0;j<selectionCount;++j) { selected.push_back(Read<unsigned>(in)); }
            std::vector<double> gammas;
            MimoChannelMatrix channel(tones,streams,streams);
            for (unsigned sc=0;sc<tones;++sc)
            {
                for (unsigned s=0;s<streams;++s)
                {
                    const auto gamma=Read<double>(in);
                    if (!std::isfinite(gamma) || gamma<0) { throw std::runtime_error("Invalid SINR"); }
                    gammas.push_back(gamma);
                    channel.Set(sc,s,s,{std::sqrt(gamma),0});
                }
            }
            auto abstraction=CreateObject<HeSuMimoLinkAbstraction>();
            const auto tx=Tx(mcs,streams,ldpc);
            const auto result=abstraction->Calculate(channel,Budget(streams),tx,bytes,selected);
            const auto& post=result.GetMimoResult();
            std::vector<double> postSelected;
            std::vector<uint32_t> indices=selected;
            if (indices.empty()) { for (unsigned sc=0;sc<tones;++sc) { indices.push_back(sc); } }
            for (auto sc:indices)
            { for (unsigned s=0;s<streams;++s) { postSelected.push_back(post.GetSinrLinear(sc,s)); } }
            const auto direct=HeEffectiveSinrMapping::MeanUncodedBer(postSelected,mcs);

            // Two independent users, each potentially carrying multiple streams.
            // The second user's 10 dB lower SINRs detect accidental user pooling.
            MultiUserChannelSnapshot snapshot{NanoSeconds(1),MultiUserChannelDirection::DOWNLINK,{}};
            std::vector<int> frequencies;
            for (unsigned sc=0;sc<tones;++sc) { frequencies.push_back(sc); }
            for (unsigned u=0;u<2;++u)
            {
                MimoChannelMatrix h(tones,streams,2*streams);
                for (unsigned sc=0;sc<tones;++sc)
                { for (unsigned s=0;s<streams;++s)
                  { h.Set(sc,s,u*streams+s,{std::sqrt(gammas[sc*streams+s]*(u?0.1:1.0)),0}); } }
                snapshot.users.push_back({u+1,100,u+1,MultiUserChannelDirection::DOWNLINK,
                    snapshot.csiTimestamp,{streams,1,0.5,0.5,"iso","V"},frequencies,0,std::move(h)});
            }
            std::vector<std::complex<double>> identity(4*streams*streams);
            std::vector<DlMuMimoStreamMapping> mappings;
            for (unsigned s=0;s<2*streams;++s)
            { identity[s*2*streams+s]={1,0}; mappings.push_back({1+s/streams,s%streams}); }
            const DlMuMimoPrecodingMatrix precoder(
                MimoPrecodingMatrix::FrequencyFlat(tones,2*streams,2*streams,identity),mappings);
            auto dl=CreateObject<HeDlMuMimoLinkAbstraction>();
            const auto mu=dl->Calculate(snapshot,precoder,
                {{2,Budget(2*streams),tx,bytes,SU_STA_ID,0,selected},
                 {1,Budget(2*streams),tx,bytes,SU_STA_ID,0,selected}});
            out << (i?",":"") << "{\"id\":\"" << id << "\",\"sample_ber\":[";
            for (unsigned j=0;j<gammas.size();++j)
            { out << (j?",":"") << HeEffectiveSinrMapping::UncodedBer(gammas[j],mcs); }
            out << "],\"post_sinr\":[";
            for (unsigned sc=0;sc<tones;++sc)
            { for (unsigned s=0;s<streams;++s)
              { out << ((sc||s)?",":"") << post.GetSinrLinear(sc,s); } }
            out << "],\"mean_ber\":" << direct.meanUncodedBer
                << ",\"effective\":" << result.GetEffectiveSinrLinear()
                << ",\"per\":" << result.GetPacketErrorRate()
                << ",\"dl_effective\":" << mu.GetUserById(1).GetEffectiveSinrLinear()
                << ",\"dl_per\":" << mu.GetUserById(1).GetPacketErrorRate()
                << ",\"dl2_effective\":" << mu.GetUserById(2).GetEffectiveSinrLinear()
                << ",\"dl2_per\":" << mu.GetUserById(2).GetPacketErrorRate() << '}';
        }
        in >> std::ws;
        if (!in.eof()) { throw std::runtime_error("Trailing input"); }
        out << "]}\n";
        if (!out) { throw std::runtime_error("Output write failed"); }
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
