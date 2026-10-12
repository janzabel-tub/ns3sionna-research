/* SPDX-License-Identifier: GPL-2.0-only */
#include "he-effective-sinr-mapping.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace ns3
{
namespace
{
void Validate(double value)
{
    if (!std::isfinite(value) || value < 0)
    {
        throw std::invalid_argument("HE mapping requires finite nonnegative symbol SINR");
    }
}
// Neumaier summation also handles the signed coefficients of the full QAM series.
class Sum
{
  public:
    void Add(double x)
    {
        const double next = m_sum + x;
        m_correction += std::abs(m_sum) >= std::abs(x) ? (m_sum - next) + x
                                                     : (x - next) + m_sum;
        m_sum = next;
    }
    double Get() const { return m_sum + m_correction; }
  private:
    double m_sum{0};
    double m_correction{0};
};
// Exact Gray square-QAM series (Cho/Yoon), combined by repeated Q argument.
// sqrt(3 log2(M) Eb/N0 / (M-1)) becomes sqrt(3 gamma / (M-1)).
std::vector<double> Coefficients(unsigned axisBits)
{
    const unsigned axis = 1u << axisBits;
    std::vector<double> terms(axis - 1, 0);
    for (unsigned bit = 1; bit <= axisBits; ++bit)
    {
        const unsigned scale = 1u << (bit - 1);
        for (unsigned i = 0; i < axis - (axis >> bit); ++i)
        {
            const unsigned parity = (i * scale) / axis;
            const int coefficient = static_cast<int>(scale) -
                                    static_cast<int>((2 * i * scale + axis) / (2 * axis));
            terms[i] += (parity % 2 ? -1.0 : 1.0) * coefficient * 2.0 / (axis * axisBits);
        }
    }
    return terms;
}
const std::array<std::vector<double>, 4> QAM =
    {Coefficients(2), Coefficients(3), Coefficients(4), Coefficients(5)};
}
uint16_t HeEffectiveSinrMapping::GetConstellationSize(uint8_t mcs)
{
    static constexpr std::array<uint16_t, 12> sizes{2,4,4,16,16,64,64,64,256,256,1024,1024};
    if (mcs >= sizes.size()) { throw std::invalid_argument("HE MCS must be in [0,11]"); }
    return sizes[mcs];
}
double HeEffectiveSinrMapping::UncodedBer(double gamma, uint8_t mcs)
{
    Validate(gamma);
    const auto size = GetConstellationSize(mcs);
    if (gamma == 0) { return 0.5; }
    if (size == 2) { return 0.5 * std::erfc(std::sqrt(gamma)); }
    if (size == 4) { return 0.5 * std::erfc(std::sqrt(gamma / 2)); }
    const unsigned index = size == 16 ? 0 : size == 64 ? 1 : size == 256 ? 2 : 3;
    const auto& coefficients = QAM[index];
    const double argument = std::sqrt(gamma) * std::sqrt(1.5 / (size - 1));
    Sum sum;
    for (unsigned i = 0; i < coefficients.size(); ++i)
    {
        if (coefficients[i] != 0)
        {
            sum.Add(coefficients[i] * 0.5 * std::erfc((2 * i + 1) * argument));
        }
    }
    return std::clamp(sum.Get(), 0.0, 0.5);
}
HeEffectiveSinrResult HeEffectiveSinrMapping::MeanUncodedBer(
    const std::vector<double>& sinrs, uint8_t mcs)
{
    GetConstellationSize(mcs);
    if (sinrs.empty()) { throw std::invalid_argument("HE mapping requires data SINR samples"); }
    double low = std::numeric_limits<double>::max(), high = 0;
    Sum sum;
    for (double gamma : sinrs)
    {
        Validate(gamma);
        low = std::min(low, gamma);
        high = std::max(high, gamma);
        sum.Add(UncodedBer(gamma, mcs));
    }
    const double mean = sum.Get() / sinrs.size();
    if (low == high || mean == 0) { return {mean, low}; }
    double left = std::log1p(low), right = std::log1p(high);
    for (unsigned i = 0; i < 80; ++i)
    {
        const double middle = left + (right - left) / 2;
        if (UncodedBer(std::expm1(middle), mcs) > mean) { left = middle; }
        else { right = middle; }
    }
    return {mean, std::clamp(std::expm1(left + (right - left) / 2), low, high)};
}
}
