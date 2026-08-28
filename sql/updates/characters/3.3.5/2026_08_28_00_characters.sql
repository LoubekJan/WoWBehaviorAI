--
-- AIWorld: AgentGroup coalition/profile provenance (Milestone 2.12E4C2,
-- STATIC review P2 fix on 1deb4b2f9b18). AgentGroupRecord::Kind
-- (LOOSE/STABLE) alone was being used by AIWorldMgr::RunCoalitionMaintenance()
-- to decide which CoalitionFormationProfile's own automatic maintenance
-- rules (LeaveRadius, MinMembers) apply to a group - wrong, since Kind is
-- shared by every profile of that Kind (today only WolfLoose forms LOOSE
-- groups, but nothing about Kind alone stops a future second LOOSE-forming
-- profile from colliding the same way), and a manually/admin-created LOOSE
-- group would otherwise silently inherit WolfLoose's own automatic
-- leave/dissolve rules with no way to opt out.
--
-- profile_id is a new, persistent identity separate from kind: 0 (Invalid,
-- see CoalitionFormationProfileId.h) means no automatic formation profile
-- created this group - the correct, fail-closed value for every group that
-- predates this column, and for any group created manually/by an admin
-- tool from here on. A nonzero value names the specific
-- CoalitionFormationProfile (e.g. 1 = WolfLoose) whose own
-- RequestCreateGroup() call actually minted this group - only then is it
-- eligible for that profile's own automatic maintenance.
--

ALTER TABLE `ai_agent_groups`
    ADD COLUMN `profile_id` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `version`;
