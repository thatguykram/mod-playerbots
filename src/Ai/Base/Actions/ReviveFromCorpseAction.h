/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REVIVEFROMCORPSEACTION_H
#define PLAYERBOTS_REVIVEFROMCORPSEACTION_H

#include "MovementActions.h"

class PlayerbotAI;
class Player;

struct GraveyardStruct;

// Is (x, y, z) in deep (fatigue) water: a body there is lost at sea.
bool InDeepWater(Player* bot, uint32 mapId, float x, float y, float z);
// Can the bot come back to life at (x, y, z): not in deep water, and not among mobs far above it.
bool BotCanReviveAt(Player* bot, uint32 mapId, float x, float y, float z);
// The graveyard nearest the inn the bot's hearthstone is bound to.
GraveyardStruct const* HomeGraveyard(Player* bot);

class ReviveFromCorpseAction : public MovementAction
{
public:
    ReviveFromCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "revive from corpse") {}

    bool Execute(Event event) override;
};

class FindCorpseAction : public MovementAction
{
public:
    FindCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "find corpse") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class SpiritHealerAction : public MovementAction
{
public:
    SpiritHealerAction(PlayerbotAI* botAI, std::string const name = "spirit healer") : MovementAction(botAI, name) {}

    GraveyardStruct const* GetGrave(bool startZone);
    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
