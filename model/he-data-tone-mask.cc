/* SPDX-License-Identifier: GPL-2.0-only */
#include "he-data-tone-mask.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace ns3
{
namespace
{
std::vector<int> Pilots(uint16_t width, unsigned family)
{
    if (width == 20 && family == 26) { return {10,22,36,48,62,76,90,102,116}; }
    if (width == 20 && family == 106) { return {22,48,90,116}; }
    if (width == 40 && family == 26) { return {10,24,36,50,64,78,90,104,116,130,144,158,170,184,198,212,224,238}; }
    if (width == 40 && family == 106) { return {10,36,78,104,144,170,212,238}; }
    if (width == 80 && family == 26) { return {10,24,38,50,64,78,92,104,118,130,144,158,172,184,198,212,226,238,252,266,280,292,306,320,334,346,360,372,386,400,414,426,440,454,468,480,494}; }
    if (width == 80 && family == 106) { return {24,50,92,118,158,184,226,252,266,292,334,360,400,426,468,494}; }
    if (width == 80 && family == 996) { return {24,92,158,226,266,334,400,468}; }
    throw std::invalid_argument("Unsupported HE tone layout");
}
std::vector<int> Active(uint16_t width, HeRu::RuType type, unsigned index)
{
    std::vector<int> tones;
    // ns-3.40's 2x996 range includes the internal 80 MHz guards/DC gaps.
    if (type == HeRu::RU_2x996_TONE)
    {
        if (width != 160 || index != 1) { throw std::invalid_argument("Invalid 2x996 RU"); }
        for (int shift : {-512, 512})
        {
            for (int tone = -500; tone <= 500; ++tone)
            { if (std::abs(tone) >= 3) { tones.push_back(tone + shift); } }
        }
    }
    else
    {
        for (const auto& range : HeRu::GetSubcarrierGroup(width, type, index))
        { for (int tone = range.first; tone <= range.second; ++tone) { tones.push_back(tone); } }
    }
    return tones;
}
bool IsPilot(uint16_t width, HeRu::RuType type, int tone)
{
    if (width == 160) { tone += tone < 0 ? 512 : -512; width = 80; }
    const unsigned family = type <= HeRu::RU_52_TONE ? 26 :
                            type <= HeRu::RU_484_TONE ? 106 : 996;
    const auto pilots = Pilots(width, family);
    return std::binary_search(pilots.begin(), pilots.end(), std::abs(tone));
}
}
std::vector<int> HeDataToneMask::GetDataTones(uint16_t width, HeRu::RuType type, unsigned index)
{
    auto tones = Active(width, type, index);
    tones.erase(std::remove_if(tones.begin(), tones.end(), [=](int tone) {
        return IsPilot(width, type, tone);
    }), tones.end());
    return tones;
}
std::vector<uint32_t> HeDataToneMask::GetDataBins(uint16_t width, HeRu::RuType type,
                                               unsigned index, uint32_t centerBin)
{
    std::vector<uint32_t> bins;
    for (int tone : GetDataTones(width, type, index))
    {
        const int64_t bin = static_cast<int64_t>(centerBin) + tone;
        if (bin < 0 || bin > UINT32_MAX) { throw std::invalid_argument("Invalid HE center bin"); }
        bins.push_back(static_cast<uint32_t>(bin));
    }
    return bins;
}
std::vector<uint32_t> HeDataToneMask::Select(uint16_t width, HeRu::RuType type,
    unsigned index, uint32_t gridSize, const std::vector<uint32_t>& occupied)
{
    const unsigned fft = width * 256 / 20;
    if (gridSize == fft || gridSize == 3 * fft)
    {
        const auto dataBins = GetDataBins(width, type, index, gridSize / 2);
        if (occupied.empty()) { return dataBins; }
        std::vector<uint32_t> selected;
        for (uint32_t bin : occupied)
        {
            if (std::binary_search(dataBins.begin(), dataBins.end(), bin))
            { selected.push_back(bin); }
        }
        if (selected.empty()) { throw std::invalid_argument("HE allocation contains no data samples"); }
        return selected;
    }
    if (occupied.empty())
    { throw std::invalid_argument("HE PHY tone selection requires FFT/spectrum layout or data indices"); }
    const auto active = Active(width, type, index);
    if (occupied.size() != active.size()) { return occupied; }
    if (!std::is_sorted(occupied.begin(), occupied.end()))
    { throw std::invalid_argument("Occupied RU bins must be in frequency order"); }
    std::vector<uint32_t> data;
    for (unsigned i = 0; i < active.size(); ++i)
    { if (!IsPilot(width, type, active[i])) { data.push_back(occupied[i]); } }
    return data;
}
}
