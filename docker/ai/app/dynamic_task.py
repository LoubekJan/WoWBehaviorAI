"""Milestone 2.13A2: Python wire contract for the /dynamic-task boundary.

This is the Python mirror of the C++ DTO contract added in 2.13A1
(src/server/game/AIWorld/Inference/Quest*.h, DynamicTask*.h) - same field
set, same field-by-field trust rules, same bounds. It exists so ai-server
can parse a request and validate a model's draft output without ever
constructing worldserver's own C++ types, exactly the same "same wire
shape, independent implementation on each side" relationship /decision's
DecisionRequest/DecisionResponse already have between AIClient.cpp and
main.py.

Every model here forbids unknown fields, rejects NaN/Infinity, and
(field-by-field, see _STRICT_MODEL_CONFIG below) refuses type coercion on
every int/float and caps them at their C++ counterpart's uint32/uint64
upper bound - a request or a model draft that doesn't match this contract
exactly must fail closed, never be silently widened, coerced or truncated
onto a value the C++ side couldn't represent. QuestObjectiveType is
intentionally restricted to the one supported value: unlike the C++ enum
(which also declares Invalid as an in-memory default), the wire contract
has no reason to accept "INVALID" as a legal value coming from the model -
anything other than a genuinely supported objective is simply an invalid
draft.
"""
from __future__ import annotations

from enum import Enum
from typing import List

from pydantic import BaseModel, ConfigDict, Field

# Milestone 2.13A1's DynamicTaskProtocolVersion::V1 - kept independent of
# /decision's CURRENT_PROTOCOL_VERSION on purpose (see
# DynamicTaskProtocolVersion.h).
CURRENT_DYNAMIC_TASK_PROTOCOL_VERSION = 1

# Mirrors QuestContractLimits.h. Every bound below is enforced on the
# model classes themselves (Field(max_length=...)), not just documented -
# a request or draft that violates one fails pydantic validation before
# any application code sees it.
QUEST_CONTRACT_MAX_RELEVANT_EVENTS = 8
QUEST_CONTRACT_MAX_CANDIDATE_TARGETS = 16
QUEST_CONTRACT_MAX_DISPLAY_NAME_LENGTH = 64
QUEST_CONTRACT_MAX_TITLE_LENGTH = 80
QUEST_CONTRACT_MAX_DESCRIPTION_LENGTH = 400

# Upper bounds for the C++ side's uint32/uint64 fields - Python's int has
# no such ceiling, so without these a value no C++ peer could ever
# represent would still pass validation here.
UINT32_MAX = 2**32 - 1
UINT64_MAX = 2**64 - 1

_STRICT_MODEL_CONFIG = ConfigDict(extra="forbid", allow_inf_nan=False)

# Every int/float field below additionally sets strict=True: pydantic's
# default lax mode still silently coerces e.g. the JSON string "3" into
# the int 3, which is the opposite of "strict structured output". This is
# deliberately a per-field override rather than model-wide
# ConfigDict(strict=True): strict mode's python-object validation for str
# Enum fields (WorldEventType/QuestObjectiveType) requires an actual enum
# member, not a same-valued plain str - which would reject every ordinary
# Python dict (the shape every call site in this codebase, and every test
# fixture, actually builds) even though the string is exactly correct.
# Numeric fields don't have that ambiguity, so strict=True is safe (and
# required) there without forcing every caller onto model_validate_json().


class WorldEventType(str, Enum):
    """Wire strings match WorldEventType.h's ToString() exactly."""

    CREATURE_KILLED = "CREATURE_KILLED"
    NPC_INJURED = "NPC_INJURED"
    PLAYER_SEEN = "PLAYER_SEEN"
    ITEM_STOLEN = "ITEM_STOLEN"
    TRADE_COMPLETED = "TRADE_COMPLETED"
    LIVESTOCK_KILLED = "LIVESTOCK_KILLED"
    WOLF_PACK_MOVED = "WOLF_PACK_MOVED"
    FOOD_SHORTAGE = "FOOD_SHORTAGE"
    NPC_DIED = "NPC_DIED"


class QuestObjectiveType(str, Enum):
    """Milestone 2.13A1 ships only the first vertical slice. Add another
    value only alongside its own authoritative 2.13B validator.
    """

    KILL_CREATURE = "KILL_CREATURE"


class QuestProblemContext(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    type: WorldEventType
    actor_entry: int = Field(strict=True, ge=0, le=UINT32_MAX)
    target_entry: int = Field(strict=True, ge=0, le=UINT32_MAX)
    map_id: int = Field(strict=True, ge=0, le=UINT32_MAX)
    age_ms: int = Field(strict=True, ge=0, le=UINT32_MAX)


class QuestRelevantEvent(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    type: WorldEventType
    actor_entry: int = Field(strict=True, ge=0, le=UINT32_MAX)
    target_entry: int = Field(strict=True, ge=0, le=UINT32_MAX)
    importance: float
    relevance: float
    age_ms: int = Field(strict=True, ge=0, le=UINT32_MAX)


class QuestTargetCandidate(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    token: int = Field(strict=True, ge=0, le=UINT32_MAX)
    entry: int = Field(strict=True, ge=0, le=UINT32_MAX)
    display_name: str = Field(max_length=QUEST_CONTRACT_MAX_DISPLAY_NAME_LENGTH)
    map_id: int = Field(strict=True, ge=0, le=UINT32_MAX)
    distance_yards: float = Field(strict=True, ge=0)
    observation_age_ms: int = Field(strict=True, ge=0, le=UINT32_MAX)


class QuestProposalLimits(BaseModel):
    """Server-declared policy window for this request's draft. Carried on
    QuestContext so the model is told its allowed range instead of having
    to guess one.
    """

    model_config = _STRICT_MODEL_CONFIG

    max_required_count: int = Field(strict=True, ge=0, le=UINT32_MAX)
    max_range_yards: float = Field(strict=True, ge=0)
    max_expiry_ms: int = Field(strict=True, ge=0, le=UINT32_MAX)
    max_reward_money_copper: int = Field(strict=True, ge=0, le=UINT32_MAX)


class QuestContext(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    agent_id: int = Field(strict=True, ge=0, le=UINT64_MAX)
    snapshot_sequence: int = Field(strict=True, ge=0, le=UINT64_MAX)
    problem: QuestProblemContext
    relevant_events: List[QuestRelevantEvent] = Field(
        default_factory=list, max_length=QUEST_CONTRACT_MAX_RELEVANT_EVENTS
    )
    candidate_targets: List[QuestTargetCandidate] = Field(
        default_factory=list, max_length=QUEST_CONTRACT_MAX_CANDIDATE_TARGETS
    )
    limits: QuestProposalLimits


class DynamicTaskRequest(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    protocol_version: int = Field(strict=True, ge=0, le=UINT32_MAX)
    request_id: int = Field(strict=True, ge=0, le=UINT64_MAX)
    context: QuestContext


class QuestProposalDraft(BaseModel):
    """Untrusted structured model output - see QuestProposalDraft.h. Named
    Draft there for the same reason it matters here: nothing in this class
    authorizes gameplay, it only proves the model's output is well-shaped
    and inside the policy window it was handed. Whether it's actually
    inside that window for one specific request is checked separately by
    validate_draft_against_context() below, since that check needs the
    QuestContext this draft was drafted against.
    """

    model_config = _STRICT_MODEL_CONFIG

    objective: QuestObjectiveType
    target_token: int = Field(strict=True, ge=0, le=UINT32_MAX)

    # A draft proposing to kill/gather zero of something, expire
    # immediately, or accept any range at all isn't a meaningful task -
    # reject those degenerate shapes here rather than downstream.
    required_count: int = Field(strict=True, gt=0, le=UINT32_MAX)
    max_range_yards: float = Field(strict=True, gt=0)
    expiry_ms: int = Field(strict=True, gt=0, le=UINT32_MAX)
    reward_money_copper: int = Field(strict=True, ge=0, le=UINT32_MAX)

    title: str = Field(max_length=QUEST_CONTRACT_MAX_TITLE_LENGTH)
    description: str = Field(max_length=QUEST_CONTRACT_MAX_DESCRIPTION_LENGTH)


class DynamicTaskResponse(BaseModel):
    model_config = _STRICT_MODEL_CONFIG

    protocol_version: int = Field(strict=True, ge=0, le=UINT32_MAX)
    request_id: int = Field(strict=True, ge=0, le=UINT64_MAX)
    agent_id: int = Field(strict=True, ge=0, le=UINT64_MAX)
    snapshot_sequence: int = Field(strict=True, ge=0, le=UINT64_MAX)
    proposal: QuestProposalDraft


class QuestProposalPolicyError(Exception):
    """Raised when a schema-valid QuestProposalDraft still violates the
    QuestProposalLimits or target-token binding its own originating
    QuestContext declared. Distinct from a schema failure: the shape is
    fine, the values are not - the model was given a policy window and a
    fixed set of legal target tokens, and it stepped outside them.
    """


def validate_draft_against_context(draft: QuestProposalDraft, context: QuestContext) -> None:
    known_tokens = {candidate.token for candidate in context.candidate_targets}
    if draft.target_token not in known_tokens:
        raise QuestProposalPolicyError(
            f"target_token {draft.target_token} is not one of this request's candidate_targets"
        )

    limits = context.limits
    if draft.required_count > limits.max_required_count:
        raise QuestProposalPolicyError("required_count exceeds this request's max_required_count")
    if draft.max_range_yards > limits.max_range_yards:
        raise QuestProposalPolicyError("max_range_yards exceeds this request's max_range_yards limit")
    if draft.expiry_ms > limits.max_expiry_ms:
        raise QuestProposalPolicyError("expiry_ms exceeds this request's max_expiry_ms limit")
    if draft.reward_money_copper > limits.max_reward_money_copper:
        raise QuestProposalPolicyError(
            "reward_money_copper exceeds this request's max_reward_money_copper limit"
        )
