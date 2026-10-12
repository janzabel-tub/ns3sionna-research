/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HE_BER_ERROR_RATE_MODEL_H
#define HE_BER_ERROR_RATE_MODEL_H
#include "ns3/error-rate-model.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"
#include <map>
#include <vector>
namespace ns3
{
/** Receiver-local HE 1x1 payload mapping. Physical PSD still supplies CCA/headers.
 * Tone snapshots are scaled to each InterferenceHelper chunk's scalar SINR.
 * The base model receives native MPDU/chunk bit counts, preserving FEC behavior.
 */
class HeBerErrorRateModel : public ErrorRateModel
{
  public:
    static TypeId GetTypeId();
    void Configure(Ptr<ErrorRateModel> base);
    Ptr<ErrorRateModel> GetBaseModel() const;
    void SetReception(uint64_t uid, const std::vector<double>& toneSnrs,
                      double scalarSnr, Time duration);
    std::size_t GetReceptionCount() const;
    double GetPayloadChunkSuccessRate(WifiMode mode, const WifiTxVector& txVector,
        double snr, uint64_t nbits, uint8_t numRxAntennas, uint16_t staId,
        const Ptr<const WifiPpdu>& ppdu, double chunkFraction) const override;
    bool IsAwgn() const override;
    int64_t AssignStreams(int64_t stream) override;
  private:
    double DoGetChunkSuccessRate(WifiMode mode, const WifiTxVector& txVector,
        double snr, uint64_t nbits, uint8_t numRxAntennas, WifiPpduField field,
        uint16_t staId) const override;
    struct Reception { std::vector<double> tones; double scalar; };
    Ptr<ErrorRateModel> m_base;
    std::map<uint64_t, Reception> m_receptions;
    /** PPDU UID, mean uncoded BER, effective SINR, coded chunk success, native bits. */
    TracedCallback<uint64_t, double, double, double, uint64_t> m_applied;
};
}
#endif
