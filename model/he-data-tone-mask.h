/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HE_DATA_TONE_MASK_H
#define HE_DATA_TONE_MASK_H
#include "ns3/he-ru.h"
#include <cstdint>
#include <vector>
namespace ns3
{
/** HE data tones, relative to channel DC. PHY RU index is in frequency order. */
class HeDataToneMask
{
  public:
    static std::vector<int> GetDataTones(uint16_t width, HeRu::RuType type, unsigned phyIndex);
    static std::vector<uint32_t> GetDataBins(uint16_t width, HeRu::RuType type,
                                           unsigned phyIndex, uint32_t centerBin);
    /** A full occupied RU list is filtered by position. Explicit smaller lists
     * are already data samples. Empty lists require an FFT or ns-3 spectrum grid.
     */
    static std::vector<uint32_t> Select(uint16_t width, HeRu::RuType type,
        unsigned phyIndex, uint32_t gridSize, const std::vector<uint32_t>& occupied);
};
}
#endif
