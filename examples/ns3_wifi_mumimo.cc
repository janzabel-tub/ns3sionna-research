/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/sionna-helper.h"
#include "ns3/sionna-mimo-helper.h"
#include "ns3/sionna-spectrum-propagation-loss-model.h"
#include "ns3/spectrum-module.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/wifi-module.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace ns3;
namespace {
constexpr uint32_t kGrid = 768;
constexpr double kPi = 3.14159265358979323846;
struct Result { std::string method, channel, timing; std::array<uint64_t,2> bytes{}, attempts{}, failures{}; std::array<double,2> sinr{}, modelPer{}, desired{}, iui{}; double airtime{}; double power{}; };
enum class Method { SU, FIXED, ZF, RZF };
std::string Name(Method m) { return m == Method::SU ? "su-round-robin-mrt" : m == Method::FIXED ? "mu-fixed" : m == Method::ZF ? "mu-zf" : "mu-rzf"; }
MultiUserChannelSnapshot Snapshot(Time t, bool correlated, double rotationHz) {
  MultiUserChannelSnapshot s{t, MultiUserChannelDirection::DOWNLINK,{}};
  const double theta=2*kPi*rotationHz*t.GetSeconds(); const double phi=correlated ? 0.2 : kPi;
  for (uint32_t id=1; id<=2; ++id) { MimoChannelMatrix h(kGrid,1,2); for(uint32_t k=0;k<kGrid;++k) { h.Set(k,0,0,{1,0}); h.Set(k,0,1,std::polar(1.0,theta+(id==2?phi:0))); } std::vector<int> f(kGrid); for(uint32_t k=0;k<kGrid;++k) f[k]=k; s.users.push_back({id,0,id,MultiUserChannelDirection::DOWNLINK,t,{1,1,0.5,0.5,"iso","V"},std::move(f),60.0,std::move(h)}); }
  return s;
}
DlMuMimoRuntimeConfiguration Runtime(Method method,double alpha,bool aged) {
  DlMuMimoRuntimeConfiguration c; c.groupingMode=DlMuMimoGroupingMode::FIXED; c.requestedUserIds={1,2}; c.timing.updateInterval=aged?MilliSeconds(5):Seconds(0); c.timing.feedbackDelay=aged?MilliSeconds(2):Seconds(0);
  if(method==Method::FIXED) { c.precoder=DlMuMimoRuntimePrecoder::FIXED; const double q=1/std::sqrt(2.0); c.fixedPrecoding=DlMuMimoPrecodingMatrix(MimoPrecodingMatrix::FrequencyFlat(kGrid,2,2,{{q,0},{q,0},{q,0},{-q,0}}),{{1,0},{2,0}}); }
  else if(method==Method::RZF) { c.precoder=DlMuMimoRuntimePrecoder::REGULARIZED_ZERO_FORCING; c.rzfAlpha=alpha; } else { c.precoder=DlMuMimoRuntimePrecoder::ZERO_FORCING; }
  return c;
}
SionnaMimoConfiguration Configuration(Method method,uint8_t mcs,double beta,double alpha,bool aged) {
  SionnaMimoConfiguration c; c.mode=SionnaMimoMode::MU_MIMO; c.txArray.numColumns=2; c.numTxAntennas=2; c.rxArray.numColumns=1; c.numRxAntennas=1; c.maxNss=1; c.receiverNoiseFigureDb=7; c.eesmBetaLinear.emplace(mcs,beta); c.dlMu=Runtime(method,alpha,aged); return c;
}
class TraceCollector
{
  public:
    explicit TraceCollector(Result* result)
        : m_result(result)
    {
    }

    void OnMu(uint64_t, uint32_t userId, uint32_t, uint32_t, HeDlMuMimoUserResult result)
    {
        const auto index = userId - 1;
        ++m_result->attempts.at(index);
        m_result->sinr.at(index) = result.GetEffectiveSinrLinear();
        m_result->modelPer.at(index) = result.GetPacketErrorRate();
        const auto& numerical = result.GetMimoResult();
        m_result->desired.at(index) = numerical.GetDesiredSignalPower(0, 0);
        m_result->iui.at(index) = numerical.GetInterUserInterferencePower(0, 0);
    }

    void OnSu(uint64_t, uint32_t, uint32_t receiverNodeId, double sinr, double per, uint32_t)
    {
        if (m_result->method != "su-round-robin-mrt" || receiverNodeId < 1 || receiverNodeId > 2)
        {
            return;
        }
        const auto index = receiverNodeId - 1;
        ++m_result->attempts.at(index);
        m_result->sinr.at(index) = sinr;
        m_result->modelPer.at(index) = per;
    }

    void OnTx(WifiConstPsduMap psdus, WifiTxVector tx, double powerW)
    {
        if (tx.GetPreambleType() == WIFI_PREAMBLE_HE_MU ||
            tx.GetPreambleType() == WIFI_PREAMBLE_HE_SU)
        {
            m_result->airtime +=
                WifiPhy::CalculateTxDuration(psdus, tx, WIFI_PHY_BAND_5GHZ).GetSeconds();
            m_result->power = powerW;
        }
    }

  private:
    Result* m_result;
};
class ReceiverTraceCollector
{
  public:
    ReceiverTraceCollector(Result* result, uint32_t userIndex)
        : m_result(result),
          m_userIndex(userIndex)
    {
    }

    void OnDrop(Ptr<const Packet> packet, WifiPhyRxfailureReason)
    {
        WifiMacHeader header;
        if (packet && packet->PeekHeader(header) != 0 && header.IsQosData())
        {
            ++m_result->failures.at(m_userIndex);
        }
    }

  private:
    Result* m_result;
    uint32_t m_userIndex;
};

Result Run(Method method,bool correlated,bool aged,uint8_t mcs,double beta,double alpha,double rotationHz) {
  RngSeedManager::SetSeed(7); RngSeedManager::SetRun(1); Result r{Name(method),correlated?"correlated":"orthogonal",aged?"aged":"current"};
  NodeContainer nodes; nodes.Create(3); NodeContainer ap(nodes.Get(0)), stas; stas.Add(nodes.Get(1)); stas.Add(nodes.Get(2)); MobilityHelper mobility; mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel"); mobility.Install(nodes); for(uint32_t i=0;i<3;++i) nodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(i+1.0,0,0));
  auto channel=CreateObject<MultiModelSpectrumChannel>(); auto loss=CreateObject<ConstantSpectrumPropagationLossModel>(); loss->SetLossDb(60); channel->AddSpectrumPropagationLossModel(loss); channel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>()); auto model=CreateObject<SionnaSpectrumPropagationLossModel>(); const auto initialSnapshot = Snapshot(Seconds(0), correlated, rotationHz); model->SetFixedChannelMatrix(initialSnapshot.users.front().channel, 60.0); model->SetPrecodingMatrix(MimoPrecodingMatrix::MaximumRatioTransmission(initialSnapshot.users.front().channel)); channel->AddSpectrumPropagationLossModel(model);
  const auto configuration=Configuration(method,mcs,beta,alpha,aged); SionnaHelper sionna("","inproc://ns3sionna-mumimo"); SpectrumWifiPhyHelper apPhy,staPhy; SionnaMimoHelper(configuration).ApplyForHeMcs(mcs,sionna,apPhy,staPhy,model); model->SetPrecodingMatrix(MimoPrecodingMatrix::MaximumRatioTransmission(initialSnapshot.users.front().channel));
  auto phy=[&channel](SpectrumWifiPhyHelper& p) { p.SetChannel(channel); p.SetErrorRateModel("ns3::TableBasedErrorRateModel"); p.Set("ChannelSettings",StringValue("{36, 20, BAND_5GHZ, 0}")); p.Set("TxPowerStart",DoubleValue(0)); p.Set("TxPowerEnd",DoubleValue(0)); }; phy(apPhy); phy(staPhy);
  WifiHelper wifi; wifi.SetStandard(WIFI_STANDARD_80211ax); wifi.ConfigHeOptions("GuardInterval",TimeValue(NanoSeconds(800))); wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager","DataMode",StringValue("HeMcs"+std::to_string(mcs)),"ControlMode",StringValue("OfdmRate24Mbps")); WifiMacHelper mac; Ssid ssid("ns3-wifi-mumimo"); mac.SetType("ns3::StaWifiMac","Ssid",SsidValue(ssid),"ActiveProbing",BooleanValue(false)); auto staDevices=wifi.Install(staPhy,mac,stas); mac.SetType("ns3::ApWifiMac","Ssid",SsidValue(ssid)); auto apDevices=wifi.Install(apPhy,mac,ap);
  for (uint32_t i = 0; i < staDevices.GetN(); ++i) {
    DynamicCast<WifiNetDevice>(staDevices.Get(i))->GetMac()->SetAttribute("BE_MaxAmsduSize", UintegerValue(0));
    DynamicCast<WifiNetDevice>(staDevices.Get(i))->GetMac()->SetAttribute("BE_MaxAmpduSize", UintegerValue(0));
    DynamicCast<WifiNetDevice>(staDevices.Get(i))->GetMac()->SetAttribute("BE_BlockAckThreshold", UintegerValue(1));
  }
  DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetMac()->SetAttribute("BE_MaxAmsduSize", UintegerValue(0));
  DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetMac()->SetAttribute("BE_MaxAmpduSize", UintegerValue(0));
  DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetMac()->SetAttribute("BE_BlockAckThreshold", UintegerValue(1));
  SionnaMimoHelper helper(configuration); const auto provider=[correlated,rotationHz](Time t){return Snapshot(t,correlated,rotationHz);}; helper.InstallDownlink(DynamicCast<WifiNetDevice>(apDevices.Get(0)),{{1,DynamicCast<WifiNetDevice>(staDevices.Get(0))},{2,DynamicCast<WifiNetDevice>(staDevices.Get(1))}},model,provider,method==Method::SU?SionnaMimoDownlinkScheduling::SU_ROUND_ROBIN:SionnaMimoDownlinkScheduling::MU); TraceCollector traces(&r); ReceiverTraceCollector sta1Drops(&r, 0); ReceiverTraceCollector sta2Drops(&r, 1); model->TraceConnectWithoutContext("DlMuMimoRx",MakeCallback(&TraceCollector::OnMu,&traces)); model->TraceConnectWithoutContext("MimoRx",MakeCallback(&TraceCollector::OnSu,&traces)); DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetPhy()->TraceConnectWithoutContext("PhyTxPsduBegin",MakeCallback(&TraceCollector::OnTx,&traces)); DynamicCast<WifiNetDevice>(staDevices.Get(0))->GetPhy()->TraceConnectWithoutContext("PhyRxDrop",MakeCallback(&ReceiverTraceCollector::OnDrop,&sta1Drops)); DynamicCast<WifiNetDevice>(staDevices.Get(1))->GetPhy()->TraceConnectWithoutContext("PhyRxDrop",MakeCallback(&ReceiverTraceCollector::OnDrop,&sta2Drops));
  InternetStackHelper internet; internet.Install(nodes); Ipv4AddressHelper addresses; addresses.SetBase("10.1.0.0","255.255.255.0"); addresses.Assign(apDevices); auto staIf=addresses.Assign(staDevices); auto apIpv4=ap.Get(0)->GetObject<Ipv4L3Protocol>(); const int32_t apInterface=apIpv4->GetInterfaceForDevice(apDevices.Get(0)); if(apInterface<0) { throw std::runtime_error("AP Wi-Fi device has no IPv4 interface"); } auto arpCache=apIpv4->GetInterface(apInterface)->GetArpCache(); for(uint32_t i=0;i<2;++i) { auto entry=arpCache->Add(staIf.GetAddress(i)); entry->SetMacAddress(staDevices.Get(i)->GetAddress()); entry->MarkPermanent(); } ApplicationContainer sinks,sources; for(uint32_t i=0;i<2;++i) { PacketSinkHelper sink("ns3::UdpSocketFactory",InetSocketAddress(Ipv4Address::GetAny(),9000+i)); sinks.Add(sink.Install(stas.Get(i))); OnOffHelper source("ns3::UdpSocketFactory",InetSocketAddress(staIf.GetAddress(i),9000+i)); source.SetAttribute("DataRate",DataRateValue(DataRate("200Mbps"))); source.SetAttribute("PacketSize",UintegerValue(1200)); source.SetAttribute("OnTime",StringValue("ns3::ConstantRandomVariable[Constant=1]")); source.SetAttribute("OffTime",StringValue("ns3::ConstantRandomVariable[Constant=0]")); sources.Add(source.Install(ap.Get(0))); }
  sinks.Start(Seconds(0)); sources.Start(Seconds(0.5)); sources.Stop(Seconds(1.5)); Simulator::Stop(Seconds(1.6)); Simulator::Run(); for(uint32_t i=0;i<2;++i) r.bytes.at(i)=DynamicCast<PacketSink>(sinks.Get(i))->GetTotalRx(); Simulator::Destroy(); return r;
}
void Print(const Result& r,uint8_t mcs) { double sum=0,square=0; for(uint32_t i=0;i<2;++i) { double gp=r.bytes.at(i)*8/1e6; sum+=gp; square+=gp*gp; double per=r.attempts.at(i)?double(r.failures.at(i))/r.attempts.at(i):0; std::cout<<"record=user method="<<r.method<<" channel="<<r.channel<<" timing="<<r.timing<<" user="<<i+1<<" delivered_bytes="<<r.bytes.at(i)<<" goodput_Mbps="<<gp<<" attempted="<<r.attempts.at(i)<<" failed="<<r.failures.at(i)<<" empirical_PER="<<per<<" model_PER="<<r.modelPer.at(i)<<" stream_SINR_linear="<<r.sinr.at(i)<<" normalized_desired_power="<<r.desired.at(i)<<" normalized_iui_power="<<r.iui.at(i)<<" mcs="<<unsigned(mcs)<<" ru=242 nss=1"<<std::endl; } const double fairness=square?sum*sum/(2*square):0; const double normalized=r.airtime?sum/r.airtime:0; std::cout<<"record=group method="<<r.method<<" channel="<<r.channel<<" timing="<<r.timing<<" ap_power_W="<<r.power<<" data_airtime_s="<<r.airtime<<" airtime_normalized_goodput_Mbps="<<normalized<<" jain_fairness="<<fairness<<std::endl; }
} // namespace
int main(int argc, char* argv[])
{
    uint8_t mcs = 5;
    double beta = std::numeric_limits<double>::quiet_NaN();
    double alpha = std::numeric_limits<double>::quiet_NaN();
    double rotationHz = 25;
    bool verify = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("mcs", "HE MCS", mcs);
    cmd.AddValue("eesmBetaLinear", "Required caller-supplied EESM beta", beta);
    cmd.AddValue("rzfAlpha", "Required positive RZF alpha", alpha);
    cmd.AddValue("rotationHz", "Channel rotation in Hz", rotationHz);
    cmd.AddValue("verify", "Run deterministic acceptance checks", verify);
    cmd.Parse(argc, argv);
    if (mcs > 11 || !std::isfinite(beta) || beta <= 0 || !std::isfinite(alpha) ||
        alpha <= 0 || !std::isfinite(rotationHz) || rotationHz < 0)
    {
        std::cerr << "Provide --eesmBetaLinear=<positive> and --rzfAlpha=<positive>; neither "
                     "has a calibration default"
                  << std::endl;
        return 2;
    }
    std::cout << std::fixed << std::setprecision(9);
    std::vector<Result> results;
    for (bool correlated : {false, true})
    {
        for (bool aged : {false, true})
        {
            for (Method method : {Method::SU, Method::FIXED, Method::ZF, Method::RZF})
            {
                results.push_back(Run(method, correlated, aged, mcs, beta, alpha, rotationHz));
                Print(results.back(), mcs);
            }
        }
    }
    if (!verify)
    {
        return 0;
    }
    const auto find = [&results](const std::string& method, const std::string& channel,
                                 const std::string& timing) -> const Result& {
        const auto result = std::find_if(results.begin(), results.end(), [&](const Result& value) {
            return value.method == method && value.channel == channel && value.timing == timing;
        });
        if (result == results.end())
        {
            throw std::runtime_error("missing deterministic MU-MIMO result");
        }
        return *result;
    };
    const auto& su = find("su-round-robin-mrt", "orthogonal", "current");
    const auto& zf = find("mu-zf", "orthogonal", "current");
    const auto& correlatedZf = find("mu-zf", "correlated", "current");
    const auto& agedZf = find("mu-zf", "orthogonal", "aged");
    const bool powerConsistent = std::all_of(results.begin(), results.end(), [](const Result& value) {
        return std::abs(value.power - 1e-3) < 1e-12;
    });
    const double suNormalized = (su.bytes[0] + su.bytes[1]) * 8e-6 / su.airtime;
    const double zfNormalized = (zf.bytes[0] + zf.bytes[1]) * 8e-6 / zf.airtime;
    const bool valid = powerConsistent && zf.attempts[0] > 0 && zf.attempts[1] > 0 &&
                       zfNormalized >= 1.5 * suNormalized &&
                       correlatedZf.sinr[0] < zf.sinr[0] && correlatedZf.sinr[1] < zf.sinr[1] &&
                       agedZf.iui[0] > 0 && agedZf.iui[1] > 0 &&
                       agedZf.sinr[0] < zf.sinr[0] && agedZf.sinr[1] < zf.sinr[1];
    std::cout << "validation=" << (valid ? "passed" : "failed")
              << " methods=4 channels=2 timings=2 deterministic_seed=7" << std::endl;
    return valid ? 0 : 1;
}
