/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AIWORLD_DYNAMICTASKJSONCODEC_H
#define AIWORLD_DYNAMICTASKJSONCODEC_H

#include "Define.h"
#include "DynamicTaskRequest.h"
#include "DynamicTaskResponse.h"

#include <string>
#include <string_view>

// Milestone 2.13A3: permanent, escape-aware JSON codec for exactly the
// /dynamic-task wire schema (DynamicTaskRequest/DynamicTaskResponse) -
// not a general-purpose JSON library (this project's pinned Boost 1.74
// predates Boost.JSON, added in 1.75 - see AIClient.cpp's own comment on
// its /decision hand-rolled JSON for the same constraint).
//
// /decision's FindStringField()/FindObjectField() in AIClient.cpp are
// deliberately NOT reused here: that parser assumes no value ever
// contains a literal '"', '{', '}' or backslash, which holds for
// /decision's fixed, enum-only schema but does not hold here -
// QuestTargetCandidate::DisplayName (request) and QuestProposalDraft::
// Title/Description (response - untrusted model output) are genuine free
// text that can contain any of those characters. Every string this codec
// touches is escaped on the way out and unescaped (with full backslash/
// \uXXXX support) on the way in - see DynamicTaskJsonCodec.cpp.
std::string SerializeDynamicTaskRequest(DynamicTaskRequest const& request);

// Parses `json` into `response`, re-validating everything the Python side
// (docker/ai/app/dynamic_task.py) already enforces server-side at the
// schema level: the body must be legal JSON per RFC 8259 (matched
// braces/brackets, legal separators, escape-aware strings, strict number
// grammar, no duplicate keys - see DynamicTaskJsonCodec.cpp's own
// two-layer grammar-then-schema design); the root object and the nested
// "proposal" object must each have *exactly* their declared field set as
// *direct* members - no missing field, no unknown/extra field, and (this
// matters specifically) a field is only ever read from the object it is
// actually declared on, never merely "found somewhere in the document" -
// the same "extra=forbid, every field required" contract ai-server's own
// pydantic models enforce; and every value must be the right JSON type
// with a known objective, uint32/uint64 bounds, finite floats (including
// floats a finite double would overflow to +-inf when narrowed to
// float), title/description length caps, and required_count/
// max_range_yards/expiry_ms > 0. A well-formed-looking body that
// violates any of those is rejected exactly like a malformed one - this
// is schema/shape validation only, the same tier /decision's own parser
// already does for its schema, not the request-specific
// QuestProposalLimits/candidate-token cross-check (that's the caller's
// job once it has both this response and the QuestContext/
// QuestRequestProvenance the request was built from - see AIWorldMgr's
// dynamic-task acceptance path).
//
// Returns false (response left completely untouched - never a partial
// fill) on any parse or validation failure. A successful parse only means
// the body was well-formed and in-contract; callers must still check the
// envelope (protocol_version/request_id/agent_id/snapshot_sequence)
// against what was actually requested - see AIClient.cpp's
// DynamicTaskSession, which does exactly that immediately after calling
// this.
bool ParseDynamicTaskResponse(std::string_view json, DynamicTaskResponse& response);

#endif // AIWORLD_DYNAMICTASKJSONCODEC_H
