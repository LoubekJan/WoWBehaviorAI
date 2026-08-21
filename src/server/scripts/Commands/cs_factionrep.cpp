/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option) any later version.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Player.h"
#include "RBAC.h"
#include "ReputationMgr.h"

class factionrep_commandscript : public CommandScript
{
public:
    factionrep_commandscript() : CommandScript("factionrep_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> factionRepCommandTable =
        {
            { "set", rbac::RBAC_PERM_COMMAND_MODIFY_REPUTATION, false, &HandleFactionRepSetCommand, "" },
            { "add", rbac::RBAC_PERM_COMMAND_MODIFY_REPUTATION, false, &HandleFactionRepAddCommand, "" },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "factionrep", rbac::RBAC_PERM_COMMAND_MODIFY_REPUTATION, false, nullptr, "", factionRepCommandTable },
        };
        return commandTable;
    }

private:
    static bool ModifyFactionReputation(ChatHandler* handler, char const* args, bool incremental)
    {
        if (!args || !*args)
            return false;

        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendSysMessage(LANG_PLAYER_NOT_FOUND);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return false;

        char* mutableArgs = strdup(args);
        char* factionTxt = strtok(mutableArgs, " ");
        char* amountTxt = strtok(nullptr, " ");

        if (!factionTxt || !amountTxt)
        {
            free(mutableArgs);
            return false;
        }

        uint32 factionId = uint32(atoi(factionTxt));
        int32 amount = int32(atoi(amountTxt));
        free(mutableArgs);

        FactionEntry const* factionEntry = sFactionStore.LookupEntry(factionId);
        if (!factionEntry)
        {
            handler->PSendSysMessage(LANG_COMMAND_FACTION_UNKNOWN, factionId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        target->GetReputationMgr().SetOneFactionReputation(factionEntry, amount, incremental);

        // Only normal client reputations have a ReputationListID that can be sent to the client.
        if (FactionState const* state = target->GetReputationMgr().GetState(factionEntry))
            target->GetReputationMgr().SendState(state);

        handler->PSendSysMessage(LANG_COMMAND_MODIFY_REP, factionEntry->Name[handler->GetSessionDbcLocale()], factionId,
            handler->GetNameLink(target).c_str(), target->GetReputationMgr().GetReputation(factionEntry));
        return true;
    }

    static bool HandleFactionRepSetCommand(ChatHandler* handler, char const* args)
    {
        return ModifyFactionReputation(handler, args, false);
    }

    static bool HandleFactionRepAddCommand(ChatHandler* handler, char const* args)
    {
        return ModifyFactionReputation(handler, args, true);
    }
};

void AddSC_factionrep_commandscript()
{
    new factionrep_commandscript();
}
