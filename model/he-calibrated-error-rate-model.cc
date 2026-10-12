/* SPDX-License-Identifier: GPL-2.0-only */
#include "he-calibrated-error-rate-model.h"

#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-ppdu.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"
#include "ns3/yans-error-rate-model.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ns3
{
namespace
{
constexpr const char* PILOT_CURVE_HASH =
    "88874ab037c840a022d3801681d1b875839e09c118487eaf898aff269b4a8656";

bool IsHash(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void ValidateBaseModel(Ptr<ErrorRateModel> baseModel)
{
    if (!baseModel || baseModel->GetInstanceTypeId() != TableBasedErrorRateModel::GetTypeId())
    {
        throw std::invalid_argument("HE pilot requires the default ns-3.40 TableBasedErrorRateModel");
    }
    UintegerValue threshold;
    PointerValue fallback;
    baseModel->GetAttribute("SizeThreshold", threshold);
    baseModel->GetAttribute("FallbackErrorRateModel", fallback);
    if (threshold.Get() != 400 || !fallback.Get<ErrorRateModel>() ||
        fallback.Get<ErrorRateModel>()->GetInstanceTypeId() != YansErrorRateModel::GetTypeId())
    {
        throw std::invalid_argument("HE pilot error-model attributes differ from the fitted curve");
    }
}
} // namespace

HeCalibrationProfile
HeCalibrationProfile::Load(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
    {
        throw std::invalid_argument("Cannot read HE calibration profile: " + path);
    }
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    google::protobuf::Struct root;
    if (!google::protobuf::util::JsonStringToMessage(json, &root).ok())
    {
        throw std::invalid_argument("Invalid HE calibration JSON: " + path);
    }
    const auto field = [](const google::protobuf::Struct& object, const std::string& name)
        -> const google::protobuf::Value& {
        const auto it = object.fields().find(name);
        if (it == object.fields().end())
        {
            throw std::invalid_argument("Missing HE calibration field: " + name);
        }
        return it->second;
    };
    const auto string = [&field](const google::protobuf::Struct& object, const std::string& name) {
        const auto& value = field(object, name);
        if (value.kind_case() != google::protobuf::Value::kStringValue)
        {
            throw std::invalid_argument("HE calibration field must be a string: " + name);
        }
        return value.string_value();
    };
    const auto number = [&field](const google::protobuf::Struct& object, const std::string& name) {
        const auto& value = field(object, name);
        if (value.kind_case() != google::protobuf::Value::kNumberValue ||
            !std::isfinite(value.number_value()))
        {
            throw std::invalid_argument("HE calibration field must be finite: " + name);
        }
        return value.number_value();
    };
    if (string(root, "status") != "validated" || string(root, "source_kind") != "link_level")
    {
        throw std::invalid_argument("HE calibration requires validated independent link-level data");
    }
    const auto& reasons = field(root, "reasons");
    const auto& phy = field(root, "phy");
    if (reasons.kind_case() != google::protobuf::Value::kListValue ||
        reasons.list_value().values_size() != 0 ||
        phy.kind_case() != google::protobuf::Value::kStructValue)
    {
        throw std::invalid_argument("Invalid HE calibration verdict or PHY metadata");
    }
    const auto& context = phy.struct_value();
    const auto integer = [&number, &context](const std::string& name, double maximum) {
        const double value = number(context, name);
        if (value < 0 || value > maximum || value != std::floor(value))
        {
            throw std::invalid_argument("Invalid HE calibration integer: " + name);
        }
        return static_cast<uint32_t>(value);
    };
    const auto coding = string(context, "coding");
    if (coding != "BCC" && coding != "LDPC")
    {
        throw std::invalid_argument("Unknown HE calibration coding");
    }
    HeCalibrationProfile profile{
        {static_cast<uint8_t>(integer("mcs", 11)), coding == "LDPC",
         integer("packet_size_bytes", UINT32_MAX),
         static_cast<uint8_t>(integer("nss", 255)),
         static_cast<uint8_t>(integer("num_rx_antennas", 255)),
         static_cast<uint16_t>(integer("channel_width_mhz", 160)),
         static_cast<uint16_t>(integer("guard_interval_ns", 3200))},
        number(root, "beta_linear"), number(root, "awgn_snr_offset_db"),
        string(root, "source"), string(root, "source_version"),
        string(root, "reference_sha256"), string(root, "awgn_model_sha256")};
    profile.Validate();
    return profile;
}

void
HeCalibrationProfile::Validate() const
{
    if (key.mcs != 0 || key.ldpc || key.packetSizeBytes != 32 || key.nss != 1 ||
        key.numRxAntennas != 1 || key.channelWidthMhz != 20 || key.guardIntervalNs != 800 ||
        !std::isfinite(betaLinear) || betaLinear <= 0 || !std::isfinite(offsetDb) ||
        std::abs(offsetDb) > 30 || source.empty() || sourceVersion.empty() ||
        !IsHash(referenceSha256) || awgnModelSha256 != PILOT_CURVE_HASH)
    {
        throw std::invalid_argument("Unsupported or invalid HE pilot calibration contract");
    }
}

NS_OBJECT_ENSURE_REGISTERED(HeCalibratedErrorRateModel);

TypeId
HeCalibratedErrorRateModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HeCalibratedErrorRateModel")
        .SetParent<ErrorRateModel>()
        .SetGroupName("Sionna")
        .AddConstructor<HeCalibratedErrorRateModel>()
        .AddTraceSource("CalibrationApplied", "Matched HE PSDU calibration at the payload boundary",
                        MakeTraceSourceAccessor(&HeCalibratedErrorRateModel::m_applied),
                        "ns3::HeCalibratedErrorRateModel::CalibrationAppliedCallback");
    return tid;
}

void
HeCalibratedErrorRateModel::Configure(const HeCalibrationProfile& profile,
                                    Ptr<ErrorRateModel> baseModel)
{
    profile.Validate();
    ValidateBaseModel(baseModel);
    auto eesm = CreateObject<HeSuMimoLinkAbstraction>();
    eesm->SetEffectiveSinrModel(SionnaMimoEffectiveSinrModel::EESM);
    eesm->SetEesmBeta(profile.key.mcs, profile.betaLinear);
    m_profile = profile;
    m_base = baseModel;
    m_eesm = eesm;
    m_receptions.clear();
}

bool
HeCalibratedErrorRateModel::Matches(const WifiTxVector& txVector, const WifiPsdu& psdu,
                                  uint8_t numRxAntennas) const
{
    return m_base && txVector.GetPreambleType() == WIFI_PREAMBLE_HE_SU &&
           txVector.GetModeInitialized() &&
           txVector.GetMode().GetModulationClass() == WIFI_MOD_CLASS_HE &&
           txVector.GetMode().GetMcsValue() == m_profile.key.mcs &&
           txVector.IsLdpc() == m_profile.key.ldpc &&
           txVector.GetNss() == m_profile.key.nss &&
           numRxAntennas == m_profile.key.numRxAntennas &&
           txVector.GetChannelWidth() == m_profile.key.channelWidthMhz &&
           txVector.GetGuardInterval() == m_profile.key.guardIntervalNs &&
           !psdu.IsAggregate() && psdu.GetNMpdus() == 1 &&
           psdu.GetSize() == m_profile.key.packetSizeBytes;
}

void
HeCalibratedErrorRateModel::SetReception(uint64_t uid, const std::vector<double>& toneSnrs,
                                       double scalarSnr, Time duration)
{
    if (!m_base || toneSnrs.size() != 234 || !std::isfinite(scalarSnr) || scalarSnr <= 0 ||
        !duration.IsStrictlyPositive() ||
        std::any_of(toneSnrs.begin(), toneSnrs.end(), [](double v) { return !std::isfinite(v) || v < 0; }))
    {
        throw std::invalid_argument("Invalid HE pilot reception snapshot");
    }
    if (!m_receptions.emplace(uid, Reception{toneSnrs, scalarSnr}).second)
    {
        throw std::invalid_argument("Duplicate HE pilot reception UID");
    }
    // EndOfMpdu runs at the PPDU end; retain the context through that event.
    Simulator::Schedule(duration + NanoSeconds(1), [self = Ptr<HeCalibratedErrorRateModel>(this), uid]() {
        self->m_receptions.erase(uid);
    });
}

double
HeCalibratedErrorRateModel::GetPayloadChunkSuccessRate(
    WifiMode mode, const WifiTxVector& txVector, double snr, uint64_t nbits,
    uint8_t numRxAntennas, uint16_t staId, const Ptr<const WifiPpdu>& ppdu,
    double chunkFraction) const
{
    const auto psdu = ppdu && txVector.GetPreambleType() == WIFI_PREAMBLE_HE_SU
                          ? ppdu->GetPsdu() : nullptr;
    const auto reception = ppdu ? m_receptions.find(ppdu->GetUid()) : m_receptions.end();
    if (staId != SU_STA_ID || !psdu || !Matches(txVector, *psdu, numRxAntennas) ||
        reception == m_receptions.end())
    {
        return GetChunkSuccessRate(mode, txVector, snr, nbits, numRxAntennas,
                                   WIFI_PPDU_FIELD_DATA, staId);
    }
    if (!std::isfinite(snr) || snr < 0 || !std::isfinite(chunkFraction) ||
        chunkFraction <= 0 || chunkFraction > 1)
    {
        throw std::invalid_argument("Invalid HE calibrated payload chunk");
    }
    ValidateBaseModel(m_base);
    auto tones = reception->second.toneSnrs;
    for (auto& value : tones)
    {
        value *= snr / reception->second.scalarSnr;
    }
    const double effective = m_eesm->CalculateEffectiveSinr(tones, m_profile.key.mcs);
    const double corrected = effective * WifiLinkBudget::DbToLinear(m_profile.offsetDb);
    const double success = m_base->GetChunkSuccessRate(mode, txVector, corrected,
                                                      8ULL * psdu->GetSize(), numRxAntennas,
                                                      WIFI_PPDU_FIELD_DATA, staId);
    m_applied(ppdu->GetUid(), effective, corrected, success, chunkFraction);
    return std::pow(success, chunkFraction);
}

double
HeCalibratedErrorRateModel::DoGetChunkSuccessRate(
    WifiMode mode, const WifiTxVector& txVector, double snr, uint64_t nbits,
    uint8_t numRxAntennas, WifiPpduField field, uint16_t staId) const
{
    if (!m_base)
    {
        throw std::logic_error("HE calibrated error model has not been configured");
    }
    return m_base->GetChunkSuccessRate(mode, txVector, snr, nbits, numRxAntennas, field, staId);
}

bool HeCalibratedErrorRateModel::IsAwgn() const { return m_base ? m_base->IsAwgn() : true; }
int64_t HeCalibratedErrorRateModel::AssignStreams(int64_t stream)
{
    return m_base ? m_base->AssignStreams(stream) : 0;
}
} // namespace ns3
