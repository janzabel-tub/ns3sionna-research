/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef SIONNA_MIMO_HELPER_H
#define SIONNA_MIMO_HELPER_H

#include "ns3/mimo-csi-feedback.h"
#include "ns3/he-su-mimo-link-abstraction.h"
#include "ns3/he-calibrated-error-rate-model.h"
#include "ns3/dl-mu-mimo-runtime.h"
#include "ns3/mimo-precoding-matrix.h"
#include "ns3/ptr.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ns3
{
class SionnaHelper;
class SionnaSpectrumPropagationLossModel;
class SpectrumWifiPhyHelper;
class WifiNetDevice;
class MultiUserScheduler;

/** Operating modes represented by the high-level MIMO API. */
enum class SionnaMimoMode
{
    SISO,
    SU_MIMO_SPATIAL_MULTIPLEXING,
    SU_BEAMFORMING,
    MU_MIMO
};

/** Downlink scheduling policy installed by SionnaMimoHelper. */
enum class SionnaMimoDownlinkScheduling
{
    MU,
    SU_ROUND_ROBIN
};

/** Stable application user binding for one associated station device. */
struct SionnaMimoStationBinding
{
    uint32_t userId;
    Ptr<WifiNetDevice> stationDevice;
};

/** Supplies a deterministic multi-user channel snapshot for an exact time. */
using SionnaMimoDownlinkSnapshotProvider = std::function<MultiUserChannelSnapshot(Time)>;

/** Sionna antenna element patterns represented by the high-level API. */
enum class SionnaMimoAntennaPattern
{
    ISOTROPIC,
    DIPOLE,
    HALF_WAVE_DIPOLE,
    TR38901
};

/** Polarization of a Sionna planar array. */
enum class SionnaMimoPolarization
{
    VERTICAL,
    HORIZONTAL,
    DUAL
};

/** Receiver algorithms represented by the high-level API. */
enum class SionnaMimoReceiverType
{
    LINEAR_MMSE,
    ZERO_FORCING
};

/** CFR normalization conventions represented by the high-level API. */
enum class SionnaMimoChannelNormalization
{
    UNIT_AVERAGE_PAIR_POWER,
    NONE
};

/** Effective-SINR mappings represented by the high-level API. */
// Selector is declared in he-effective-sinr-mapping.h.

/** Channel-dependent SU beamforming algorithms exposed by the helper. */
enum class SionnaMimoBeamformingMethod
{
    MRT,
    SVD
};

/**
 * Explicit options for channel-dependent SU beamforming.
 *
 * MRT is restricted to current perfect MISO CSI. SVD uses the completed
 * power-allocation, SU-feedback, and CSI-timing contracts. Compressed
 * feedback and non-zero CSI timing remain equal-power-only.
 */
struct SionnaMimoBeamformingConfiguration
{
    SionnaMimoBeamformingMethod method{SionnaMimoBeamformingMethod::SVD};
    MimoSvdPrecodingOptions svd;
    MimoFeedbackConfiguration feedback;
    MimoCsiTimingConfiguration csiTiming;
};

/**
 * Geometry and element configuration of one Sionna planar array.
 *
 * Spacings are expressed in wavelengths. A dual-polarized element contributes
 * two antenna ports to the CFR tensor.
 */
struct SionnaMimoArrayConfiguration
{
    uint32_t numRows{1};
    uint32_t numColumns{1};
    double verticalSpacing{0.5};
    double horizontalSpacing{0.5};
    SionnaMimoAntennaPattern pattern{SionnaMimoAntennaPattern::ISOTROPIC};
    SionnaMimoPolarization polarization{SionnaMimoPolarization::VERTICAL};

    /** Return the number of antenna ports produced by this array. */
    uint64_t GetNumAntennas() const;
};

/** Node-specific azimuth/elevation/roll orientation in degrees. */
struct SionnaMimoOrientation
{
    double azimuthDegrees{0.0};
    double elevationDegrees{0.0};
    double rollDegrees{0.0};
};

/** Configuration contract owned by SionnaMimoHelper. */
struct SionnaMimoConfiguration
{
    SionnaMimoMode mode{SionnaMimoMode::SISO};
    SionnaMimoArrayConfiguration txArray{
        1,
        1,
        0.5,
        0.5,
        SionnaMimoAntennaPattern::TR38901,
        SionnaMimoPolarization::VERTICAL};
    SionnaMimoArrayConfiguration rxArray{
        1,
        1,
        0.5,
        0.5,
        SionnaMimoAntennaPattern::DIPOLE,
        SionnaMimoPolarization::VERTICAL};
    uint32_t numTxAntennas{1};
    uint32_t numRxAntennas{1};
    uint32_t maxNss{1};
    SionnaMimoReceiverType receiverType{SionnaMimoReceiverType::LINEAR_MMSE};
    SionnaMimoChannelNormalization channelNormalization{
        SionnaMimoChannelNormalization::UNIT_AVERAGE_PAIR_POWER};
    double receiverNoiseFigureDb{7.0};
    SionnaMimoEffectiveSinrModel effectiveSinrModel{SionnaMimoEffectiveSinrModel::MEAN_UNCODED_BER};
    std::map<uint8_t, double> eesmBetaLinear;
    /** Exact PHY contexts with measured AWGN SNR corrections; empty by default. */
    std::map<HeAwgnCalibrationKey, double> awgnSnrOffsetsDb;
    /** Optional validated pilot for explicit installation on a receiving device. */
    std::optional<HeCalibrationProfile> heCalibrationProfile;
    std::map<uint32_t, SionnaMimoOrientation> nodeOrientations;
    std::optional<MimoPrecodingMatrix> precodingMatrix;
    std::optional<SionnaMimoBeamformingConfiguration> beamforming;
    /** Required runtime grouping, CSI timing, and MU precoder configuration. */
    std::optional<DlMuMimoRuntimeConfiguration> dlMu;
};

/**
 * Immutable helper result for one channel-dependent precoder selection.
 *
 * The selected matrix always obeys the existing fixed-W[k] contract. SVD,
 * feedback, quantization-error, and CSI-age details remain available through
 * the original Phase-3 result objects rather than being reinterpreted here.
 */
class SionnaMimoPrecodingSelection
{
  public:
    SionnaMimoBeamformingMethod GetMethod() const;
    const MimoPrecodingMatrix& GetPrecodingMatrix() const;
    Time GetCsiTimestamp() const;
    Time GetTransmissionTimestamp() const;
    Time GetCsiAge() const;

    bool HasSvdResult() const;
    const MimoSvdPrecodingResult& GetSvdResult() const;

    bool HasCsiSelectionResult() const;
    const MimoCsiSelectionResult& GetCsiSelectionResult() const;

    /** Return a stream's transmitted-power fraction on one subcarrier. */
    double GetPowerFraction(uint32_t subcarrier, uint32_t stream) const;

  private:
    friend class SionnaMimoHelper;

    SionnaMimoPrecodingSelection(SionnaMimoBeamformingMethod method,
                                 MimoPrecodingMatrix precoding,
                                 Time csiTimestamp,
                                 Time transmissionTimestamp);
    explicit SionnaMimoPrecodingSelection(MimoSvdPrecodingResult svd,
                                          Time csiTimestamp,
                                          Time transmissionTimestamp);
    explicit SionnaMimoPrecodingSelection(MimoCsiSelectionResult csiSelection);

    SionnaMimoBeamformingMethod m_method;
    MimoPrecodingMatrix m_precoding;
    Time m_csiTimestamp;
    Time m_transmissionTimestamp;
    std::optional<MimoSvdPrecodingResult> m_svd;
    std::optional<MimoCsiSelectionResult> m_csiSelection;
};

/**
 * High-level ns3sionna MIMO configuration API.
 *
 * The value-based contract remains simulation-independent. Apply() forwards a
 * validated configuration to Sionna, the Phase-1 abstraction, and separate
 * transmit/receive SpectrumWifiPhy helpers.
 */
class SionnaMimoHelper
{
  public:
    /** Construct a helper with legacy-compatible SISO defaults. */
    SionnaMimoHelper();

    /**
     * Construct a helper from a complete validated configuration.
     *
     * \param configuration configuration to validate and store
     * \throws std::invalid_argument if the configuration is invalid or unsupported
     */
    explicit SionnaMimoHelper(const SionnaMimoConfiguration& configuration);

    /** Return the stored configuration. */
    const SionnaMimoConfiguration& GetConfiguration() const;

    /** Validate and atomically replace the stored configuration. */
    void SetConfiguration(const SionnaMimoConfiguration& configuration);

    /**
     * Apply the complete configuration to the Phase-1 integration objects.
     *
     * The PHY helpers are separate so asymmetric TX/RX antenna counts remain
     * representable. Existing SionnaHelper users are unaffected unless this
     * method is called.
     *
     * \param sionnaHelper existing Sionna IPC helper
     * \param txPhyHelper SpectrumWifiPhy helper for the transmitting endpoint
     * \param rxPhyHelper SpectrumWifiPhy helper for the receiving endpoint
     * \param spectrumModel Sionna SpectrumWifiPhy propagation adapter
     * \throws std::invalid_argument if the stored configuration or model is invalid
     */
    void Apply(SionnaHelper& sionnaHelper,
               SpectrumWifiPhyHelper& txPhyHelper,
               SpectrumWifiPhyHelper& rxPhyHelper,
               Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const;

    /**
     * Prevalidate an HE MCS and apply the complete configuration.
     *
     * This convenience entry point validates the used MCS before changing objects.
     * Only explicitly selected EESM requires an MCS-specific beta. General
     * reception also applies the common mapping to 1x1 payloads.
     *
     * \param heMcs HE MCS used by the configured link (0--11)
     * \param sionnaHelper existing Sionna IPC helper
     * \param txPhyHelper SpectrumWifiPhy helper for the transmitting endpoint
     * \param rxPhyHelper SpectrumWifiPhy helper for the receiving endpoint
     * \param spectrumModel Sionna SpectrumWifiPhy propagation adapter
     * \throws std::invalid_argument if the configuration, MCS, or model is invalid
     */
    void ApplyForHeMcs(uint8_t heMcs,
                       SionnaHelper& sionnaHelper,
                       SpectrumWifiPhyHelper& txPhyHelper,
                       SpectrumWifiPhyHelper& rxPhyHelper,
                       Ptr<SionnaSpectrumPropagationLossModel> spectrumModel) const;

    /**
     * Install an AP-owned deterministic HE downlink scheduler after devices
     * exist and before Simulator::Run().  Association IDs are resolved by the
     * AP when a transmission is scheduled.
     */
    Ptr<MultiUserScheduler> InstallDownlink(
        Ptr<WifiNetDevice> apDevice,
        const std::vector<SionnaMimoStationBinding>& stationBindings,
        Ptr<SionnaSpectrumPropagationLossModel> spectrumModel,
        SionnaMimoDownlinkSnapshotProvider snapshotProvider,
        SionnaMimoDownlinkScheduling scheduling) const;

    /** Validate and set the orientation of one ns-3 node ID. */
    void SetNodeOrientation(uint32_t nodeId, const SionnaMimoOrientation& orientation);

    /** Remove a node-specific orientation, restoring the all-zero default. */
    void ClearNodeOrientation(uint32_t nodeId);

    /** Return whether a node has an explicit orientation. */
    bool HasNodeOrientation(uint32_t nodeId) const;

    /** Return a node's orientation or the all-zero default if none was set. */
    SionnaMimoOrientation GetNodeOrientation(uint32_t nodeId) const;

    /** Validate and store an explicit linear EESM beta for one HE MCS (0--11). */
    void SetEesmBeta(uint8_t mcs, double betaLinear);

    /** Validate and store a measured correction for one exact HE PHY context. */
    void SetAwgnSnrOffsetDb(const HeAwgnCalibrationKey& key, double offsetDb);

    /** Load the existing pilot atomically; preserve measurement hashes unchanged. */
    void LoadHeCalibrationProfile(const std::string& path);

    /** Install payload calibration after Apply() and device creation, before Run().
     * Reject unsupported PHY dimensions or a different configured error model.
     */
    Ptr<HeCalibratedErrorRateModel> InstallHeCalibration(Ptr<WifiNetDevice> receiver) const;

    /** Return whether an explicit EESM beta exists for an HE MCS. */
    bool HasEesmBeta(uint8_t mcs) const;

    /** Return the configured beta, throwing if the HE MCS has no calibration. */
    double GetEesmBeta(uint8_t mcs) const;

    /** Validate and atomically configure user-provided fixed precoding. */
    void SetPrecodingMatrix(const MimoPrecodingMatrix& precoding);

    /** Clear fixed precoding if the resulting no-precoding configuration remains valid. */
    void ClearPrecodingMatrix();

    /** Return whether user-provided fixed precoding is configured. */
    bool HasPrecodingMatrix() const;

    /** Return the configured fixed precoding tensor, throwing if absent. */
    const MimoPrecodingMatrix& GetPrecodingMatrix() const;

    /**
     * Select and retain an MRT or SVD precoder from an explicit CSI snapshot.
     *
     * The selected immutable W[k] is subsequently installed by Apply() through
     * the existing fixed-precoding path. For periodic CSI, csiTimestamp must be
     * the newest sample available at transmissionTimestamp according to the
     * configured update interval and feedback delay.
     *
     * \param csiTimeChannelEstimate channel estimate used only for W[k] selection
     * \param txNodeId directed transmitter identity
     * \param rxNodeId directed receiver identity
     * \param csiTimestamp timestamp of csiTimeChannelEstimate
     * \param transmissionTimestamp timestamp at which the selected W[k] is used
     * \return the retained immutable selection and original Phase-3 diagnostics
     */
    const SionnaMimoPrecodingSelection& SelectPrecoding(
        const MimoChannelMatrix& csiTimeChannelEstimate,
        uint32_t txNodeId,
        uint32_t rxNodeId,
        Time csiTimestamp,
        Time transmissionTimestamp);

    /** Clear a channel-dependent selection without changing the configuration. */
    void ClearSelectedPrecoding();

    /** Return whether channel-dependent beamforming has been selected. */
    bool HasSelectedPrecoding() const;

    /** Return the selected beamforming result, throwing if absent. */
    const SionnaMimoPrecodingSelection& GetSelectedPrecoding() const;

    /** Return the modes that are functional through Phase 4.6. */
    static const std::array<SionnaMimoMode, 4>& GetSupportedModes();

    /** Return whether a mode is functional through Phase 4.6. */
    static bool IsModeSupported(SionnaMimoMode mode);

    /**
     * Validate all mode-independent and cross-field constraints.
     *
     * No simulation objects are accessed or modified.
     */
    static void Validate(const SionnaMimoConfiguration& configuration);

    /**
     * Validate the configuration for use with a particular HE MCS.
     *
     * Explicit EESM requires a beta for every used MCS; general mode needs none.
     * SISO preserves the Phase-1 scalar path and therefore does not require
     * one.
     */
    static void ValidateForHeMcs(const SionnaMimoConfiguration& configuration, uint8_t mcs);

  private:
    SionnaMimoConfiguration m_configuration;
    std::optional<SionnaMimoPrecodingSelection> m_selectedPrecoding;
};

} // namespace ns3

#endif // SIONNA_MIMO_HELPER_H
