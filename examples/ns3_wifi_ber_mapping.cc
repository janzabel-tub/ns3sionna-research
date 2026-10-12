/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/command-line.h"
#include "ns3/he-effective-sinr-mapping.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/he-phy.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-tx-vector.h"
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using namespace ns3;
int main(int argc, char* argv[])
{
    uint32_t mcs = 0, bytes = 32;
    bool ldpc = false;
    std::string mapping = "MEAN_UNCODED_BER", samples = "10,1", reference;
    double beta = 0;
    CommandLine cmd(__FILE__);
    cmd.AddValue("mapping", "MEAN_UNCODED_BER or EESM", mapping);
    cmd.AddValue("sinrs", "Comma-separated data symbol SINRs in linear units", samples);
    cmd.AddValue("mcs", "HE MCS 0-11", mcs);
    cmd.AddValue("ldpc", "Use LDPC coding (required for MCS 10/11)", ldpc);
    cmd.AddValue("bytes", "Packet size in bytes", bytes);
    cmd.AddValue("beta", "Explicit EESM beta in linear units", beta);
    cmd.AddValue("reference", "Optional frozen HE MCS0 link-level JSON comparison", reference);
    cmd.Parse(argc, argv);
    if (mcs > 11 || !bytes || (mcs >= 10 && !ldpc) ||
        (mapping != "MEAN_UNCODED_BER" && mapping != "EESM") ||
        (mapping == "EESM" && (!std::isfinite(beta) || beta <= 0))) { return 2; }
    auto abstraction = CreateObject<HeSuMimoLinkAbstraction>();
    if (mapping == "EESM")
    { abstraction->SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel::EESM); abstraction->SetEesmBeta(mcs,beta); }
    auto base = CreateObject<TableBasedErrorRateModel>();
    const WifiMode mode = HePhy::GetHeMcs(mcs);
    WifiTxVector tx(mode,0,WIFI_PREAMBLE_HE_SU,800,1,1,0,20,false); tx.SetLdpc(ldpc);
    std::cout << std::setprecision(17) << "case,mapping,mean_uncoded_ber,effective_sinr_linear,effective_sinr_db,predicted_per,measured_per,abs_error\n";
    const auto report = [&](const std::string& name, const std::vector<double>& values, double measured) {
        const double mean = HeEffectiveSinrMapping::MeanUncodedBer(values,mcs).meanUncodedBer;
        const double effective = abstraction->CalculateEffectiveSinr(values,mcs);
        const double per = 1-base->GetChunkSuccessRate(mode,tx,effective,bytes*8ULL,1);
        std::cout << name << ',' << mapping << ',' << mean << ',' << effective << ','
                  << 10*std::log10(effective) << ',' << per << ',';
        if (measured >= 0) { std::cout << measured << ',' << std::abs(per-measured); }
        else { std::cout << ','; }
        std::cout << '\n';
    };
    if (reference.empty())
    {
        std::vector<double> values; std::istringstream input(samples); std::string token;
        while (std::getline(input,token,',')) { values.push_back(std::stod(token)); }
        report("samples",values,-1);
    }
    else
    {
        if (mcs != 0 || ldpc || bytes != 32) { return 2; }
        std::ifstream input(reference); const std::string json((std::istreambuf_iterator<char>(input)),{});
        google::protobuf::Struct root;
        if (!google::protobuf::util::JsonStringToMessage(json,&root).ok()) { return 2; }
        unsigned index = 0;
        for (const std::string category : {"awgn","selective"})
        {
            for (const auto& value : root.fields().at(category).list_value().values())
            {
                const auto& row = value.struct_value(); std::vector<double> values;
                if (category == "awgn") { values.push_back(std::pow(10.0,row.fields().at("snr_db").number_value()/10)); }
                else { for (const auto& v : row.fields().at("sinr_linear").list_value().values()) { values.push_back(v.number_value()); } }
                report(category+"_"+std::to_string(index++), values,
                       row.fields().at("errors").number_value()/row.fields().at("trials").number_value());
            }
        }
    }
}
