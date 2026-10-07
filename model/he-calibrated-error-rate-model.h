/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HE_CALIBRATED_ERROR_RATE_MODEL_H
#define HE_CALIBRATED_ERROR_RATE_MODEL_H

#include "he-su-mimo-link-abstraction.h"
#include "ns3/error-rate-model.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <map>
#include <string>
#include <vector>

namespace ns3
{
class WifiPsdu;

/** Runtime contract for the existing HE MCS 0/BCC/32-byte/1x1 pilot only.
 * Legacy fit metadata describes PSDU bytes, not application payload bytes.
 * The curve hash identifies ns-3.40's default TableBasedErrorRateModel.
 */
struct HeCalibrationProfile
{
    HeAwgnCalibrationKey key;
    double betaLinear{0};
    double offsetDb{0};
    std::string source;
    std::string sourceVersion;
    std::string referenceSha256;
    std::string awgnModelSha256;

    static HeCalibrationProfile Load(const std::string& path);
    void Validate() const;
};

/** Payload-only calibration; header and unmatched chunk calls delegate unchanged.
 * Each receiver owns its PPDU snapshots. EESM is recalculated for each scalar
 * interference level; physical PSD, CCA and header SNR remain unchanged.
 */
class HeCalibratedErrorRateModel : public ErrorRateModel
{
  public:
    static TypeId GetTypeId();
    void Configure(const HeCalibrationProfile& profile, Ptr<ErrorRateModel> baseModel);
    bool Matches(const WifiTxVector& txVector, const WifiPsdu& psdu,
                 uint8_t numRxAntennas) const;
    /** Store data-tone SNRs and the scalar SNR observed by InterferenceHelper. */
    void SetReception(uint64_t uid, const std::vector<double>& toneSnrs,
                      double scalarSnr, Time duration);
    double GetPayloadChunkSuccessRate(WifiMode mode, const WifiTxVector& txVector,
                                     double snr, uint64_t nbits, uint8_t numRxAntennas,
                                     uint16_t staId, const Ptr<const WifiPpdu>& ppdu,
                                     double chunkFraction) const override;
    bool IsAwgn() const override;
    int64_t AssignStreams(int64_t stream) override;

  private:
    double DoGetChunkSuccessRate(WifiMode mode, const WifiTxVector& txVector,
                                double snr, uint64_t nbits, uint8_t numRxAntennas,
                                WifiPpduField field, uint16_t staId) const override;
    struct Reception
    {
        std::vector<double> toneSnrs;
        double scalarSnr;
    };
    HeCalibrationProfile m_profile;
    Ptr<ErrorRateModel> m_base;
    Ptr<HeSuMimoLinkAbstraction> m_eesm;
    std::map<uint64_t, Reception> m_receptions;
    /** uid, effective SINR, corrected SINR, whole-PSDU success, window fraction. */
    TracedCallback<uint64_t, double, double, double, double> m_applied;
};
} // namespace ns3
#endif
