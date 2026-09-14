/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef HE_UL_MU_MIMO_PHY_ADAPTER_H
#define HE_UL_MU_MIMO_PHY_ADAPTER_H

#include "he-ul-mu-mimo-link-abstraction.h"

#include "ns3/he-ru.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class CtrlTriggerHeader;
class ErrorRateModel;
class HePpdu;

/** Stable association between a channel user, an HE STA-ID, and a transmitter node. */
struct HeUlMuMimoUserBinding
{
    uint32_t userId;
    uint16_t staId;
    uint32_t txNodeId;
};

/** One HE-TB PPDU that physically arrived at the AP. */
struct HeUlMuMimoPpduReception
{
    uint16_t staId;
    uint32_t txNodeId;
    Ptr<const HePpdu> ppdu;
    Time rxStart;
    Time rxEnd;
    double receivedPowerW;
    uint32_t packetSizeBytes;
    /** Occupied CFR bins (data and pilots); empty selects the full grid. */
    std::vector<uint32_t> subcarrierIndices{};
};

/** Joint-detection result for one spatially shared RU. */
class HeUlMuMimoPhyGroupResult
{
  public:
    const HeRu::RuSpec& GetRu() const;
    const HeUlMuMimoLinkResult& GetLinkResult() const;

  private:
    friend class HeUlMuMimoPhyAdapter;

    HeRu::RuSpec m_ru;
    HeUlMuMimoLinkResult m_linkResult;
};

/** Result of binding one Basic Trigger exchange to actually coincident HE-TB PPDUs. */
class HeUlMuMimoPhyResult
{
  public:
    uint32_t GetNumGroups() const;
    uint32_t GetNumDetectedUsers() const;
    const HeUlMuMimoPhyGroupResult& GetGroup(uint32_t index) const;
    const HeUlMuMimoUserResult& GetUserById(uint32_t userId) const;
    double GetEquivalentReceivedPowerW(uint32_t userId) const;
    const std::vector<uint16_t>& GetMissingStaIds() const;
    const std::vector<uint16_t>& GetNonSimultaneousStaIds() const;

  private:
    struct UserEquivalentPower
    {
        uint32_t userId;
        double powerW;
    };

    friend class HeUlMuMimoPhyAdapter;

    std::vector<HeUlMuMimoPhyGroupResult> m_groups;
    std::vector<UserEquivalentPower> m_equivalentPowers;
    std::vector<uint16_t> m_missingStaIds;
    std::vector<uint16_t> m_nonSimultaneousStaIds;
};

/**
 * \ingroup sionna
 * Adapter from coordinated HE Basic Trigger/HE-TB PHY metadata to the
 * Phase-4.7 uplink MU-MIMO numerical abstraction.
 *
 * The adapter deliberately models ideal synchronization: only receptions
 * whose start and end equal the caller-supplied coordinated interval are
 * jointly detected. Users on different RUs are evaluated independently.
 */
class HeUlMuMimoPhyAdapter : public Object
{
  public:
    static TypeId GetTypeId();

    void SetErrorRateModel(Ptr<ErrorRateModel> errorRateModel);
    Ptr<ErrorRateModel> GetErrorRateModel() const;
    void SetEesmBeta(uint8_t mcs, double betaLinear);
    void SetEesmBetaDb(uint8_t mcs, double betaDb);

    HeUlMuMimoPhyResult Calculate(
        const MultiUserChannelSnapshot& snapshot,
        const CtrlTriggerHeader& trigger,
        uint64_t triggerUid,
        const std::vector<HeUlMuMimoUserBinding>& bindings,
        const std::vector<HeUlMuMimoPpduReception>& receptions,
        Time coordinatedStart,
        Time coordinatedEnd,
        uint32_t apNodeId,
        uint8_t apRxAntennas,
        double receiverNoiseFigureDb,
        UlMuMimoDetector detector,
        double rankTolerance = UlMuMimoLinkModel::DEFAULT_RANK_TOLERANCE) const;

  private:
    Ptr<HeUlMuMimoLinkAbstraction> m_abstraction{CreateObject<HeUlMuMimoLinkAbstraction>()};
};

} // namespace ns3

#endif // HE_UL_MU_MIMO_PHY_ADAPTER_H
