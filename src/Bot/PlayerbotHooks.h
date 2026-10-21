/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTHOOKS_H
#define PLAYERBOTS_PLAYERBOTHOOKS_H

#include "Define.h"
#include "PlayerbotAIConfig.h"

class Group;
class Player;
class Quest;

// Extension points for other modules. All default to nullptr (stock behaviour). Kept out of the
// widely included headers so setting one doesn't rebuild the whole module.

// Per-bot RPG status weights (a temperament): when an idle bot picks its next status, the hook gets
// the configured weight for each candidate and returns the weight to use for this bot; 0 disables
// that status for the bot. Called on the bot's map thread.
using RpgStatusWeightHook = uint32 (*)(Player* bot, NewRpgStatus status, uint32 configured);
extern RpgStatusWeightHook g_RpgStatusWeightHook;

// Random bots leave groups led by another random bot (and every group on refresh), and leave groups
// whose leader is far away, on another map or too far apart in level. Return true to keep `bot` in
// `group`, e.g. for bot parties another module put together. Called on the world thread, and on the
// bot's map thread from "leave far away"; map updates never overlap world-thread script updates.
using KeepRandomBotGroupHook = bool (*)(Player* bot, Group* group);
extern KeepRandomBotGroupHook g_KeepRandomBotGroupHook;

// Per-bot quest taste: asked after playerbots' own level/repeatable checks whenever a bot considers
// taking or keeping a quest. Return false to skip it (or drop it from the log). Called on the bot's map
// thread.
using QuestWorthHook = bool (*)(Player* bot, Quest const* quest);
extern QuestWorthHook g_QuestWorthHook;

// Per-bot patience: how long (ms) a bot keeps at a quest objective with no progress before giving up on
// the quest. Gets playerbots' default. Called on the bot's map thread.
using QuestPatienceHook = uint32 (*)(Player* bot, uint32 configured);
extern QuestPatienceHook g_QuestPatienceHook;

// Per-bot reaction time: the pause (ms) after a non-reaction action in combat before the bot acts again;
// gets the action's own duration. Lets a module make some players slower on the buttons. Called on
// the bot's map thread.
using CombatActionDelayHook = uint32 (*)(Player* bot, uint32 duration);
extern CombatActionDelayHook g_CombatActionDelayHook;

// Per-bot destination check for RPG movement (grind spots, camps, gather nodes, wandering): return false
// when the place, or the way there from where the bot stands, is too dangerous for it. When it's set,
// a bot whose zone has no grind spot for its level may also pick one in the next zone over. Called on
// the bot's map thread.
using DestinationHook = bool (*)(Player* bot, uint32 mapId, float x, float y);
extern DestinationHook g_DestinationHook;

// Per-bot gathering reach: when the bot's zone has no node it can use (its skill outgrew the zone, or lags
// behind it), how many yards it will travel to nodes in other zones of the map; 0 = its own zone only.
// Called on the bot's map thread.
using GatherSearchHook = float (*)(Player* bot);
extern GatherSearchHook g_GatherSearchHook;

// Per-bot talent plan: gets the spec playerbots rolled (or the one the bot already has, when firstPoint is
// false) and returns the spec to use; may also clear followBuild so the bot picks talents as it goes
// instead of following the configured build. Called on level-up (map thread) and in the factory.
using TalentPlanHook = uint32 (*)(Player* bot, uint32 spec, bool firstPoint, bool& followBuild);
extern TalentPlanHook g_TalentPlanHook;

// Random bot login: asked for each character playerbots considers logging in to fill the bot count, with
// its level as stored; return false to skip it (e.g. only fresh characters join). Called on the world thread.
using RandomBotLoginHook = bool (*)(uint32 guid, uint8 level);
extern RandomBotLoginHook g_RandomBotLoginHook;

#endif
