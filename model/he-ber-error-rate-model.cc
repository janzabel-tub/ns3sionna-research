/* SPDX-License-Identifier: GPL-2.0-only */
#include "he-ber-error-rate-model.h"
#include "he-effective-sinr-mapping.h"
#include "ns3/simulator.h"
#include "ns3/wifi-ppdu.h"
#include "ns3/wifi-tx-vector.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(HeBerErrorRateModel);
TypeId HeBerErrorRateModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HeBerErrorRateModel").SetParent<ErrorRateModel>()
        .SetGroupName("Sionna").AddConstructor<HeBerErrorRateModel>()
        .AddTraceSource("MappingApplied", "HE payload BER mapping per interference chunk",
                       MakeTraceSourceAccessor(&HeBerErrorRateModel::m_applied),
                       "ns3::HeBerErrorRateModel::MappingAppliedCallback");
    return tid;
}
void HeBerErrorRateModel::Configure(Ptr<ErrorRateModel> base)
{
    if (!base || base == this || DynamicCast<HeBerErrorRateModel>(base))
    { throw std::invalid_argument("HE BER wrapper requires an underlying error model"); }
    m_base = base;
    m_receptions.clear();
}
Ptr<ErrorRateModel> HeBerErrorRateModel::GetBaseModel() const { return m_base; }
std::size_t HeBerErrorRateModel::GetReceptionCount() const { return m_receptions.size(); }
void HeBerErrorRateModel::SetReception(uint64_t uid, const std::vector<double>& tones,
                                     double scalar, Time duration)
{
    if (!m_base || tones.empty() || !std::isfinite(scalar) || scalar <= 0 ||
        !duration.IsStrictlyPositive() ||
        std::any_of(tones.begin(), tones.end(), [](double v) { return !std::isfinite(v) || v < 0; }))
    { throw std::invalid_argument("Invalid HE BER reception snapshot"); }
    if (!m_receptions.emplace(uid, Reception{tones, scalar}).second)
    { throw std::invalid_argument("Duplicate HE BER reception UID"); }
    // Keep context for EndOfMpdu, which executes at the PPDU end.
    Simulator::Schedule(duration + NanoSeconds(1), [self = Ptr<HeBerErrorRateModel>(this), uid] {
        self->m_receptions.erase(uid);
    });
}
double HeBerErrorRateModel::GetPayloadChunkSuccessRate(WifiMode mode,
    const WifiTxVector& txVector, double snr, uint64_t nbits, uint8_t antennas,
    uint16_t staId, const Ptr<const WifiPpdu>& ppdu, double chunkFraction) const
{
    const auto found = ppdu ? m_receptions.find(ppdu->GetUid()) : m_receptions.end();
    if (found == m_receptions.end() || mode.GetModulationClass() != WIFI_MOD_CLASS_HE ||
        txVector.GetPreambleType() != WIFI_PREAMBLE_HE_SU || antennas != 1 ||
        txVector.GetNss(staId) != 1)
    { return GetChunkSuccessRate(mode, txVector, snr, nbits, antennas, WIFI_PPDU_FIELD_DATA, staId); }
    if (!std::isfinite(snr) || snr < 0)
    { throw std::invalid_argument("Invalid HE payload scalar SINR"); }
    auto tones = found->second.tones;
    // Multiply by the ratio once: external interference is already in scalar snr.
    for (double& value : tones) { value *= snr / found->second.scalar; }
    const auto mapped = HeEffectiveSinrMapping::MeanUncodedBer(tones, mode.GetMcsValue());
    const double success = m_base->GetChunkSuccessRate(mode, txVector,
        mapped.effectiveSinrLinear, nbits, antennas, WIFI_PPDU_FIELD_DATA, staId);
    m_applied(ppdu->GetUid(), mapped.meanUncodedBer, mapped.effectiveSinrLinear, success, nbits);
    return success;
}
double HeBerErrorRateModel::DoGetChunkSuccessRate(WifiMode mode,
    const WifiTxVector& txVector, double snr, uint64_t nbits, uint8_t antennas,
    WifiPpduField field, uint16_t staId) const
{
    if (!m_base) { throw std::logic_error("HE BER wrapper is not configured"); }
    return m_base->GetChunkSuccessRate(mode, txVector, snr, nbits, antennas, field, staId);
}
bool HeBerErrorRateModel::IsAwgn() const { return m_base ? m_base->IsAwgn() : true; }
int64_t HeBerErrorRateModel::AssignStreams(int64_t stream)
{ return m_base ? m_base->AssignStreams(stream) : 0; }
}
