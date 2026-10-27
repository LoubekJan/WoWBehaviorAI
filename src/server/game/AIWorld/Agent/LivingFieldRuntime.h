/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGFIELDRUNTIME_H
#define AIWORLD_LIVINGFIELDRUNTIME_H

#include "AgentType.h"
#include "Action/ActionPosition.h"
#include <array>

namespace LivingFields
{
    struct FieldSpec
    {
        uint32 Id;
        uint32 Depot;
        char const* Name;
        ActionPosition DepotPoint;
        std::array<ActionPosition, 12> WorkPoints;
    };
    struct WorkerSpec { uint64 Spawn; uint32 Field; uint32 Depot; uint32 Ordinal; };

    // Crop-interior candidates from the preview-v8 terrain texture audit. These
    // are work destinations, never replacements for immutable actor homes.
    // Runtime navigation must independently prove each physical approach.
    inline constexpr std::array<FieldSpec, 3> Fields{{
        {4, 4, "Top village", {725, 378.03635207966687f, 626.4014701827371f, 0.15f}, {{
            {725, 383.59373536705971f, 566.92706170678139f, 0.0f},
            {725, 375.26040235161781f, 566.92706170678139f, 0.0f},
            {725, 367.96873596310616f, 563.28122851252556f, 0.0f},
            {725, 368.48956927657127f, 571.61456152796745f, 0.0f},
            {725, 389.32290181517601f, 572.65622815489769f, 0.0f},
            {725, 389.32290181517601f, 561.19789525866508f, 0.0f},
            {725, 379.94790217280388f, 574.21872809529305f, 0.0f},
            {725, 379.94790217280388f, 559.63539531826973f, 0.0f},
            {725, 361.71873620152473f, 558.07289537787437f, 0.0f},
            {725, 371.61456915736198f, 555.9895621240139f, 0.0f},
            {725, 361.19790288805962f, 567.96872833371162f, 0.0f},
            {725, 363.28123614192009f, 577.86456128954887f, 0.0f}
        }}},
        {5, 5, "Left village", {725, 44.97056274847714f, 859.9705627484772f, 0.15f}, {{
            {725, 37.239581912755966f, 794.01038637757301f, 0.0f},
            {725, 35.156248658895493f, 801.82288607954979f, 0.0f},
            {725, 43.489581674337387f, 788.80205324292183f, 0.0f},
            {725, 33.593748718500137f, 786.71871998906136f, 0.0f},
            {725, 42.968748360872269f, 799.73955282568932f, 0.0f},
            {725, 28.906248897314072f, 794.01038637757301f, 0.0f},
            {725, 40.885415107011795f, 807.55205252766609f, 0.0f},
            {725, 28.906248897314072f, 807.03121921420097f, 0.0f},
            {725, 49.739581435918808f, 783.59372010827065f, 0.0f},
            {725, 49.21874812245369f, 794.53121969103813f, 0.0f},
            {725, 39.843748480081558f, 781.51038685441017f, 0.0f},
            {725, 48.697914808988571f, 805.46871927380562f, 0.0f}
        }}},
        {6, 6, "Right village", {725, 151.18440237676216f, 282.17310878027087f, 0.15f}, {{
            {725, 150.78124424815178f, 347.65623673796654f, 0.0f},
            {725, 162.23957714438438f, 347.13540342450142f, 0.0f},
            {725, 143.48957785964012f, 351.30206993222237f, 0.0f},
            {725, 142.968744546175f, 342.96873691678047f, 0.0f},
            {725, 155.98957738280296f, 341.40623697638512f, 0.0f},
            {725, 167.96874359250069f, 341.40623697638512f, 0.0f},
            {725, 168.48957690596581f, 352.3437365591526f, 0.0f},
            {725, 150.26041093468666f, 355.98956975340843f, 0.0f},
            {725, 158.59374395012856f, 355.46873643994331f, 0.0f},
            {725, 137.76041141152382f, 357.03123638033867f, 0.0f},
            {725, 137.2395780980587f, 337.23957046866417f, 0.0f},
            {725, 148.69791099429131f, 337.23957046866417f, 0.0f}
        }}}
    }};
    inline constexpr std::array<WorkerSpec, 12> Workers{{
        {901016, 4, 4, 0}, {901017, 4, 4, 1}, {901018, 4, 4, 2}, {901019, 4, 4, 3},
        {901022, 5, 5, 0}, {901023, 5, 5, 1}, {901024, 5, 5, 2}, {901025, 5, 5, 3},
        {901028, 6, 6, 0}, {901029, 6, 6, 1}, {901030, 6, 6, 2}, {901031, 6, 6, 3}
    }};

    inline FieldSpec const* Field(uint32 id)
    {
        for (auto const& spec : Fields) if (spec.Id == id) return &spec;
        return nullptr;
    }
    inline WorkerSpec const* Worker(uint64 spawn)
    {
        for (auto const& spec : Workers) if (spec.Spawn == spawn) return &spec;
        return nullptr;
    }
    inline bool Capability(uint64 spawn, uint32 map, AgentType type, AgentControlMode control, bool enabled)
    {
        return enabled && map == 725 && type == AgentType::Civilian &&
            control == AgentControlMode::AIWorldControlled && Worker(spawn);
    }
    inline ActionPosition const* WorkPoint(WorkerSpec const& worker, uint64 cycle)
    {
        auto field = Field(worker.Field);
        if (!field || field->Depot != worker.Depot || worker.Ordinal >= 4)
            return nullptr;
        return &field->WorkPoints[worker.Ordinal + 4 * (cycle % 3)];
    }
}
#endif
