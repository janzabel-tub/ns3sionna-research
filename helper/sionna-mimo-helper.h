/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026
 */

#ifndef SIONNA_MIMO_HELPER_H
#define SIONNA_MIMO_HELPER_H

#include "ns3/mimo-precoding-matrix.h"
#include "ns3/ptr.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>

namespace ns3
{
class SionnaHelper;
class SionnaSpectrumPropagationLossModel;
class SpectrumWifiPhyHelper;

/** Operating modes represented by the high-level MIMO API. */
enum class SionnaMimoMode
{
    SISO,
    SU_MIMO_SPATIAL_MULTIPLEXING,
    SU_BEAMFORMING,
    MU_MIMO
};

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
enum class SionnaMimoEffectiveSinrModel
{
    EESM,
    MUTUAL_INFORMATION
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
    SionnaMimoEffectiveSinrModel effectiveSinrModel{SionnaMimoEffectiveSinrModel::EESM};
    std::map<uint8_t, double> eesmBetaLinear;
    std::map<uint32_t, SionnaMimoOrientation> nodeOrientations;
    std::optional<MimoPrecodingMatrix> precodingMatrix;
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
     * This convenience entry point rejects a missing SU-MIMO EESM beta before
     * any Sionna, PHY-helper, or propagation-model state is changed. SISO keeps
     * the legacy scalar path and therefore requires no beta.
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

    /** Return the modes that are functional through Phase 3.1. */
    static const std::array<SionnaMimoMode, 2>& GetSupportedModes();

    /** Return whether a mode is functional through Phase 3.1. */
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
     * Open-loop SU-MIMO requires an explicit EESM beta. SISO preserves the
     * Phase-1 scalar path and therefore does not require one.
     */
    static void ValidateForHeMcs(const SionnaMimoConfiguration& configuration, uint8_t mcs);

  private:
    SionnaMimoConfiguration m_configuration;
};

} // namespace ns3

#endif // SIONNA_MIMO_HELPER_H
