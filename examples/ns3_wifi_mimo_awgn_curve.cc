/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Export the exact ns-3 AWGN PER mapping used after MIMO EESM compression.
 */

#include "ns3/command-line.h"
#include "ns3/he-phy.h"
#include "ns3/table-based-error-rate-model.h"
#include "ns3/wifi-tx-vector.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>

using namespace ns3;

int
main(int argc, char* argv[])
{
    uint32_t mcs = 0;
    uint32_t packetSizeBytes = 1458;
    uint32_t nss = 1;
    uint32_t numRxAntennas = 1;
    uint32_t channelWidthMhz = 20;
    uint32_t guardIntervalNs = 800;
    bool ldpc = false;
    double snrStartDb = -20.0;
    double snrStopDb = 50.0;
    double snrStepDb = 0.25;

    CommandLine cmd(__FILE__);
    cmd.AddValue("mcs", "HE MCS, 0 through 11", mcs);
    cmd.AddValue("packetSizeBytes", "Payload length in bytes", packetSizeBytes);
    cmd.AddValue("nss", "Number of spatial streams", nss);
    cmd.AddValue("numRxAntennas", "Number of receiver antennas passed to ErrorRateModel", numRxAntennas);
    cmd.AddValue("channelWidthMhz", "Channel width in MHz", channelWidthMhz);
    cmd.AddValue("guardIntervalNs", "HE guard interval in ns", guardIntervalNs);
    cmd.AddValue("ldpc", "Use LDPC coding (otherwise BCC)", ldpc);
    cmd.AddValue("snrStartDb", "First SNR in dB", snrStartDb);
    cmd.AddValue("snrStopDb", "Last SNR in dB", snrStopDb);
    cmd.AddValue("snrStepDb", "SNR grid spacing in dB", snrStepDb);
    cmd.Parse(argc, argv);

    if (mcs > 11 || packetSizeBytes == 0 || nss == 0 || nss > 255 ||
        numRxAntennas == 0 || numRxAntennas > 255 ||
        (channelWidthMhz != 20 && channelWidthMhz != 40 && channelWidthMhz != 80 &&
         channelWidthMhz != 160) ||
        (guardIntervalNs != 800 && guardIntervalNs != 1600 && guardIntervalNs != 3200) ||
        !std::isfinite(snrStartDb) || !std::isfinite(snrStopDb) ||
        !std::isfinite(snrStepDb) || snrStepDb <= 0.0 || snrStopDb < snrStartDb ||
        (snrStopDb - snrStartDb) / snrStepDb > 100000)
    {
        std::cerr << "Invalid AWGN curve configuration" << std::endl;
        return 2;
    }

    const auto mode = HePhy::GetHeMcs(static_cast<uint8_t>(mcs));
    WifiTxVector txVector(mode,
                          0,
                          WIFI_PREAMBLE_HE_SU,
                          static_cast<uint16_t>(guardIntervalNs),
                          static_cast<uint8_t>(nss),
                          static_cast<uint8_t>(nss),
                          0,
                          static_cast<uint16_t>(channelWidthMhz),
                          false);
    txVector.SetLdpc(ldpc);
    if (!txVector.IsValid())
    {
        std::cerr << "Invalid HE SU WifiTxVector" << std::endl;
        return 2;
    }

    const auto errorModel = CreateObject<TableBasedErrorRateModel>();
    std::cout << std::setprecision(17);
    std::cout << "mcs,coding,packet_size_bytes,nss,num_rx_antennas,channel_width_mhz,"
                 "guard_interval_ns,snr_db,per\n";
    const auto count = static_cast<uint32_t>(std::floor((snrStopDb - snrStartDb) / snrStepDb)) + 1;
    for (uint32_t index = 0; index < count; ++index)
    {
        const double snrDb = snrStartDb + index * snrStepDb;
        const double snrLinear = std::pow(10.0, snrDb / 10.0);
        const double success = errorModel->GetChunkSuccessRate(
            mode,
            txVector,
            snrLinear,
            static_cast<uint64_t>(packetSizeBytes) * 8,
            static_cast<uint8_t>(numRxAntennas));
        if (!std::isfinite(success) || success < 0.0 || success > 1.0)
        {
            std::cerr << "ErrorRateModel returned an invalid probability" << std::endl;
            return 2;
        }
        std::cout << mcs << ',' << (ldpc ? "LDPC" : "BCC") << ',' << packetSizeBytes << ','
                  << nss << ',' << numRxAntennas << ',' << channelWidthMhz << ','
                  << guardIntervalNs << ',' << snrDb << ',' << 1.0 - success << '\n';
    }
    return 0;
}
