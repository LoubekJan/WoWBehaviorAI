/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __TRINITY_REPUTATION_MGR_H
#define __TRINITY_REPUTATION_MGR_H

#include "Common.h"
#include "EnumFlag.h"
#include "Language.h"
#include "DatabaseEnvFwd.h"
#include "DBCStructure.h"
#include "SharedDefines.h"
#include <set>
#include <map>

TC_GAME_API extern uint32 const ReputationRankStrIndex[MAX_REPUTATION_RANK];

enum class ReputationFlags : uint8
{
    None                        = 0x0000,
    Visible                     = 0x0001,
    AtWar                       = 0x0002,
    Hidden                      = 0x0004,
    Header                      = 0x0008,
    Peaceful                    = 0x0010,
    Inactive                    = 0x0020,
    ShowPropagated              = 0x0040,
    HeaderShowsBar              = 0x0080
};

DEFINE_ENUM_FLAG(ReputationFlags);

typedef uint32 RepListID;
struct FactionState
{
    uint32 ID;
    RepListID ReputationListID;
    int32 Standing;
    EnumFlag<ReputationFlags> Flags = ReputationFlags::None;
    bool needSend;
    bool needSave;
};

typedef std::map<RepListID, FactionState> FactionStateList;
typedef std::map<uint32, ReputationRank> ForcedReactions;

struct ServerFactionReputation
{
    int32 Standing = 0;
    bool needSave = false;
};

typedef std::map<uint32, ServerFactionReputation> ServerFactionReputationList;

class Player;

class TC_GAME_API ReputationMgr
{
    public:
        explicit ReputationMgr(Player* owner) : _player(owner),
            _visibleFactionCount(0), _honoredFactionCount(0), _reveredFactionCount(0), _exaltedFactionCount(0), _sendFactionIncreased(false) { }
        ~ReputationMgr() { }

        void SaveToDB(CharacterDatabaseTransaction trans);
        void LoadFromDB(PreparedQueryResult result);
    public:
        static std::set<int32> const ReputationRankThresholds;
        static const int32 Reputation_Cap;
        static const int32 Reputation_Bottom;

        static ReputationRank ReputationToRank(FactionEntry const* factionEntry, int32 standing);
    public:
        uint8 GetVisibleFactionCount() const { return _visibleFactionCount; }
        uint8 GetHonoredFactionCount() const { return _honoredFactionCount; }
        uint8 GetReveredFactionCount() const { return _reveredFactionCount; }
        uint8 GetExaltedFactionCount() const { return _exaltedFactionCount; }

        FactionStateList const& GetStateList() const { return _factions; }
        ServerFactionReputationList const& GetServerFactionReputations() const { return _serverFactionReputations; }

        FactionState const* GetState(FactionEntry const* factionEntry) const;

        FactionState const* GetState(RepListID id) const
        {
            FactionStateList::const_iterator repItr = _factions.find(id);
            return repItr != _factions.end() ? &repItr->second : nullptr;
        }

        bool IsAtWar(uint32 faction_id) const;
        bool IsAtWar(FactionEntry const* factionEntry) const;
        bool IsReputationAllowedForTeam(TeamId team, uint32 factionId) const;

        int32 GetReputation(uint32 faction_id) const;
        int32 GetReputation(FactionEntry const* factionEntry) const;
        int32 GetBaseReputation(FactionEntry const* factionEntry) const;
        int32 GetMinReputation(FactionEntry const* factionEntry) const;
        int32 GetMaxReputation(FactionEntry const* factionEntry) const;

        ReputationRank GetRank(FactionEntry const* factionEntry) const;
        ReputationRank GetBaseRank(FactionEntry const* factionEntry) const;
        std::string GetReputationRankName(FactionEntry const* factionEntry) const;

        ReputationRank const* GetForcedRankIfAny(FactionTemplateEntry const* factionTemplateEntry) const;
        ReputationRank const* GetForcedRankIfAny(uint32 factionId) const;

    public:
        bool SetReputation(FactionEntry const* factionEntry, int32 standing)
        {
            return SetReputation(factionEntry, standing, false, false);
        }
        bool ModifyReputation(FactionEntry const* factionEntry, int32 standing, bool spillOverOnly = false)
        {
            return SetReputation(factionEntry, standing, true, spillOverOnly);
        }

        void SetVisible(FactionTemplateEntry const* factionTemplateEntry);
        void SetVisible(FactionEntry const* factionEntry);
        void SetAtWar(RepListID repListID, bool on);
        void SetInactive(RepListID repListID, bool on);

        void ApplyForceReaction(uint32 faction_id, ReputationRank rank, bool apply);

        //! Public for chat command needs
        bool SetOneFactionReputation(FactionEntry const* factionEntry, int32 standing, bool incremental);

    public:
        void SendInitialReputations();
        void SendForceReactions();
        void SendState(FactionState const* faction);

    private:
        void Initialize();
        ReputationFlags GetDefaultStateFlags(FactionEntry const* factionEntry) const;
        bool SetReputation(FactionEntry const* factionEntry, int32 standing, bool incremental, bool spillOverOnly);
        void SetVisible(FactionState* faction);
        void SetAtWar(FactionState* faction, bool atWar) const;
        void SetInactive(FactionState* faction, bool inactive) const;
        void SendVisible(FactionState const* faction) const;
        void UpdateRankCounters(ReputationRank old_rank, ReputationRank new_rank);
        int32 GetFactionDataIndexForRaceAndClass(FactionEntry const* factionEntry) const;

    private:
        Player* _player;
        FactionStateList _factions;
        ServerFactionReputationList _serverFactionReputations;
        ForcedReactions _forcedReactions;
        uint8 _visibleFactionCount;
        uint8 _honoredFactionCount;
        uint8 _reveredFactionCount;
        uint8 _exaltedFactionCount;
        bool _sendFactionIncreased;
};

#endif
