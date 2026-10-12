/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HE_EFFECTIVE_SINR_MAPPING_H
#define HE_EFFECTIVE_SINR_MAPPING_H
#include <cstdint>
#include <vector>
namespace ns3
{
/** Stable selector values: append new mappings without renumbering EESM/MI. */
enum class SionnaMimoEffectiveSinrModel
{
    EESM = 0,
    MUTUAL_INFORMATION = 1,
    MEAN_UNCODED_BER = 2,
    LEGACY_SISO = 3
};
struct HeEffectiveSinrResult
{
    double meanUncodedBer;
    double effectiveSinrLinear;
};
/** Parameter-free Gray-labelled AWGN BER, using symbol SINR (Es/N0).
 * Numerical callers supply data samples; physical adapters select HE data tones.
 */
class HeEffectiveSinrMapping
{
  public:
    static uint16_t GetConstellationSize(uint8_t mcs);
    static double UncodedBer(double symbolSinr, uint8_t mcs);
    static HeEffectiveSinrResult MeanUncodedBer(const std::vector<double>& sinrs, uint8_t mcs);
};
}
#endif
