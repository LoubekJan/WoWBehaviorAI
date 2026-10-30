/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "NpcAgentAppearance.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Player.h"
#include "SpellAuraDefines.h"

uint32 NativeNpcAgentAppearanceDisplay(NpcAgentAppearance const& appearance)
{
    auto const* race = sChrRacesStore.LookupEntry(appearance.Race);
    if (!race || appearance.Gender > 1) return 0;
    uint32 display = appearance.Gender == 0 ? race->MaleDisplayID : race->FemaleDisplayID;
    return sCreatureDisplayInfoStore.LookupEntry(display) ? display : 0;
}

bool ValidateNativeNpcAgentAppearance(NpcAgentAppearance const& appearance)
{
    if (!appearance.SpawnId || appearance.Class != 1 || !NativeNpcAgentAppearanceDisplay(appearance) ||
        !Player::ValidateAppearance(appearance.Race, appearance.Class, appearance.Gender,
            appearance.HairStyle, appearance.HairColor, appearance.Face, appearance.FacialHair, appearance.Skin, true))
        return false;
    for (std::size_t slot = 0; slot < appearance.Equipment.size(); ++slot)
    {
        auto const& item = appearance.Equipment[slot];
        if (!item.ItemId && !item.DisplayId) continue;
        auto const* dbc = sItemStore.LookupEntry(item.ItemId);
        if (!dbc || !item.DisplayId || dbc->DisplayInfoID != item.DisplayId ||
            !NpcAppearanceSlotAccepts(NpcAppearanceSlot(slot), dbc->InventoryType)) return false;
    }
    return true;
}

bool ApplyNpcAgentAppearance(Creature& creature, NpcAgentAppearance const& appearance)
{
    if (!creature.IsInWorld() || creature.GetSpawnId() != appearance.SpawnId || creature.GetMapId() != appearance.MapId ||
        creature.IsPet() || creature.IsCharmed() || creature.IsControlledByPlayer() || !creature.GetCharmerOrOwnerGUID().IsEmpty()) return false;
    // Initial binding or a same-GUID respawn must not cancel a real polymorph,
    // shapeshift or clone already owned by the spell system. Retry after it ends.
    if (creature.HasAuraType(SPELL_AURA_TRANSFORM) || creature.HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) ||
        creature.HasAuraType(SPELL_AURA_CLONE_CASTER)) return false;
    uint32 display = NativeNpcAgentAppearanceDisplay(appearance);
    if (!display) return false;
    creature.SetRace(appearance.Race);
    creature.SetGender(Gender(appearance.Gender));
    creature.SetDisplayId(display);
    creature.SetNativeDisplayId(display);
    for (std::size_t slot = 0; slot < 3; ++slot)
        creature.SetVirtualItem(uint32(slot), appearance.Equipment[std::size_t(NpcAppearanceSlot::MainHand) + slot].ItemId);
    creature.SetUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE);
    return true;
}
