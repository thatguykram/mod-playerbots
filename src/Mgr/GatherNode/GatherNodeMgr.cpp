/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherNodeMgr.h"
#include <cmath>

#include <algorithm>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotHooks.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"

namespace
{
bool GetGatherSkillFromTemplate(GameObjectTemplate const* goInfo, uint32& skillId, uint32& reqSkillValue)
{
    if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
        return false;

    uint32 lockId = goInfo->GetLockId();
    if (!lockId)
        return false;

    LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
    if (!lockInfo)
        return false;

    for (uint8 j = 0; j < 8; ++j)
    {
        if (lockInfo->Type[j] != LOCK_KEY_SKILL)
            continue;

        uint32 skill = SkillByLockType(LockType(lockInfo->Index[j]));
        if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
        {
            skillId = skill;
            reqSkillValue = std::max(1u, lockInfo->Skill[j]);
            return true;
        }
    }

    return false;
}
}

void GatherNodeMgr::Load()
{
    uint32 oldMSTime = getMSTime();
    uint32 count = 0;

    // Loot ids with at least one freely lootable row. Quest-gated
    // "gathering" chests (Cactus Apple, Serpentbloom, ...) carry a
    // herbalism/mining lock but ALL their loot rows are QuestRequired, so
    // bots without the quest can't harvest them - and they never despawn,
    // which makes them permanent live-node bait. Note that filtering on
    // gameobject_questitem instead would be wrong: standard herbs carry a
    // conditional quest item (Root Sample) on top of their normal loot.
    std::unordered_set<uint32> openLootIds;
    if (QueryResult result =
            WorldDatabase.Query("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE QuestRequired = 0"))
    {
        do
        {
            openLootIds.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }

    std::unordered_map<ObjectGuid::LowType, uint32> dbZoneIds;
    if (QueryResult result = WorldDatabase.Query("SELECT guid, zoneId FROM gameobject WHERE zoneId <> 0"))
    {
        do
        {
            dbZoneIds[(*result)[0].Get<uint32>()] = (*result)[1].Get<uint32>();
        } while (result->NextRow());
    }

    // Danger per 40-yard cell: the highest level of creature spawned there that attacks players on
    // sight. A node takes the worst of its own cell and the eight around it.
    constexpr float DangerCell = 40.0f;
    auto cellKey = [](uint32 mapId, int32 cx, int32 cy)
    { return (uint64(mapId) << 32) | (uint64(uint16(cx + 32768)) << 16) | uint64(uint16(cy + 32768)); };
    auto cellOf = [](float coord) { return int32(std::floor(coord / DangerCell)); };
    std::unordered_map<uint64, uint8> danger;
    for (auto const& [spawnId, creatureData] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(creatureData.id);
        if (!tmpl || tmpl->type == CREATURE_TYPE_CRITTER)
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(tmpl->faction);
        if (!faction || !faction->IsHostileToPlayers())
            continue;
        uint8& level = danger[cellKey(creatureData.mapid, cellOf(creatureData.posX), cellOf(creatureData.posY))];
        level = std::max(level, tmpl->maxlevel);
    }

    for (auto const& [spawnId, goData] : sObjectMgr->GetAllGOData())
    {
        // Instances are excluded: the state targets outdoor zone roaming.
        MapEntry const* mapEntry = sMapStore.LookupEntry(goData.mapid);
        if (!mapEntry || !mapEntry->IsContinent())
            continue;

        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(goData.id);

        uint32 skillId = 0;
        uint32 reqSkillValue = 0;
        if (!GetGatherSkillFromTemplate(goInfo, skillId, reqSkillValue))
            continue;

        if (!openLootIds.contains(goInfo->GetLootId()))
            continue;

        GatherNodeSpawn node;
        node.spawnId = spawnId;
        node.pos = WorldPosition(goData.mapid, goData.posX, goData.posY, goData.posZ);
        node.skillId = skillId;
        node.reqSkillValue = reqSkillValue;
        for (int32 dx = -1; dx <= 1; ++dx)
            for (int32 dy = -1; dy <= 1; ++dy)
                if (auto it = danger.find(cellKey(goData.mapid, cellOf(goData.posX) + dx, cellOf(goData.posY) + dy));
                    it != danger.end())
                    node.dangerLevel = std::max(node.dangerLevel, it->second);
        auto zoneItr = dbZoneIds.find(spawnId);
        uint32 zoneId = zoneItr != dbZoneIds.end()
                            ? zoneItr->second
                            : sMapMgr->GetZoneId(PHASEMASK_NORMAL, goData.mapid, goData.posX, goData.posY, goData.posZ);
        _nodes[goData.mapid][zoneId].push_back(std::move(node));
        ++count;
    }

    LOG_INFO("playerbots", ">> Loaded {} gather node spawns in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

bool GatherNodeMgr::IsUsable(PlayerbotAI* botAI, Player* bot, GatherNodeSpawn const& node)
{
    if (!botAI->HasSkill(SkillType(node.skillId)))
        return false;

    if (sPlayerbotAIConfig.GatherMaxMobLevelAbove >= 0 &&
        int32(node.dangerLevel) > int32(bot->GetLevel()) + sPlayerbotAIConfig.GatherMaxMobLevelAbove)
        return false;

    if (bot->GetSkillValue(node.skillId) < node.reqSkillValue)
        return false;

    // Leash for new characters: a node is only worth it near somewhere the bot can level, or one vein
    // after another walks it out of its start area.
    if (bot->GetLevel() < sPlayerbotAIConfig.KeepInStartZoneBelowLevel)
    {
        constexpr float Leash = 800.0f;
        for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(bot->GetLevel()))
            if (loc.GetMapId() == node.pos.GetMapId() && WorldPosition(loc).sqDistance(node.pos) <= Leash * Leash)
                return true;
        return false;
    }

    return true;
}

std::vector<GatherNodeSpawn> const* GatherNodeMgr::GetZoneNodes(uint32 mapId, uint32 zoneId) const
{
    auto mapItr = _nodes.find(mapId);
    if (mapItr == _nodes.end())
        return nullptr;

    auto zoneItr = mapItr->second.find(zoneId);
    return zoneItr != mapItr->second.end() ? &zoneItr->second : nullptr;
}

GatherSearchHook g_GatherSearchHook = nullptr;

bool GatherNodeMgr::HasUsableNodes(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId()))
        for (GatherNodeSpawn const& node : *nodes)
            if (IsUsable(botAI, bot, node))
                return true;

    // Skill-up trip: nothing usable here, so look at the other zones within the bot's reach.
    float reach = g_GatherSearchHook ? g_GatherSearchHook(bot) : 0.0f;
    auto mapItr = _nodes.find(bot->GetMapId());
    if (reach <= 0.0f || mapItr == _nodes.end())
        return false;

    WorldPosition botPos(bot);
    for (auto const& [zoneId, zoneNodes] : mapItr->second)
        for (GatherNodeSpawn const& node : zoneNodes)
            if (botPos.sqDistance(node.pos) <= reach * reach && IsUsable(botAI, bot, node))
                return true;

    return false;
}

GatherNodeSpawn const* GatherNodeMgr::GetNextNode(Player* bot, std::unordered_set<ObjectGuid::LowType> const& visited)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);

    std::vector<std::pair<float, GatherNodeSpawn const*>> candidates;
    auto collect = [&](std::vector<GatherNodeSpawn> const& nodes, float maxDist)
    {
        for (GatherNodeSpawn const& node : nodes)
        {
            if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node))
                continue;

            // Don't route to spawn points we can already see to be empty.
            // Evaluated fresh on every selection so respawns come back into
            // the pool naturally.
            if (IsVerifiablyDown(map, node.pos, node.spawnId))
                continue;

            // Walking across the zone to a far node takes a low-level bot through whatever lives in between.
            float sqDist = botPos.sqDistance(node.pos);
            if (maxDist > 0.0f && sqDist > maxDist * maxDist)
                continue;

            if (g_DestinationHook &&
                !g_DestinationHook(bot, node.pos.GetMapId(), node.pos.GetPositionX(), node.pos.GetPositionY()))
                continue;

            candidates.push_back({sqDist, &node});
        }
    };

    if (std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId()))
        collect(*nodes, sPlayerbotAIConfig.GatherMaxNodeDistance);

    // Skill-up trip: nothing usable in this zone, so the other zones of the map within the bot's reach.
    float reach = g_GatherSearchHook ? g_GatherSearchHook(bot) : 0.0f;
    auto mapItr = _nodes.find(bot->GetMapId());
    if (candidates.empty() && reach > 0.0f && mapItr != _nodes.end())
        for (auto const& [zoneId, zoneNodes] : mapItr->second)
            if (zoneId != bot->GetZoneId())
                collect(zoneNodes, reach);

    if (candidates.empty())
        return nullptr;

    uint32 nearestCount = std::min<uint32>(4, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + nearestCount, candidates.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

    return candidates[urand(0, nearestCount - 1)].second;
}

GameObject* GatherNodeMgr::FindLiveNode(Map* map, ObjectGuid::LowType spawnId)
{
    auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second->isSpawned() && it->second->GetGoState() == GO_STATE_READY)
            return it->second;

    return nullptr;
}

bool GatherNodeMgr::IsVerifiablyDown(Map* map, WorldPosition const& pos, ObjectGuid::LowType spawnId)
{
    // An unloaded grid means "unknown", not "not up".
    if (!map->IsGridLoaded(pos.GetPositionX(), pos.GetPositionY()))
        return false;

    return !FindLiveNode(map, spawnId);
}

GatherNodeSpawn const* GatherNodeMgr::GetNearestLiveNode(Player* bot,
                                                         std::unordered_set<ObjectGuid::LowType> const& visited,
                                                         float radius)
{
    auto mapItr = _nodes.find(bot->GetMapId());
    if (mapItr == _nodes.end())
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);
    GatherNodeSpawn const* nearest = nullptr;
    float nearestDistSq = radius * radius;
    // All zone buckets of the map: this query is deliberately not
    // zone-scoped, and a radius this small prunes nearly everything on the
    // cheap distance check.
    for (auto const& [zoneId, nodes] : mapItr->second)
    {
        for (GatherNodeSpawn const& node : nodes)
        {
            float distSq = botPos.sqDistance(node.pos);
            if (distSq > nearestDistSq)
                continue;

            if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node))
                continue;

            if (!map->IsGridLoaded(node.pos.GetPositionX(), node.pos.GetPositionY()))
                continue;

            if (!FindLiveNode(map, node.spawnId))
                continue;

            nearest = &node;
            nearestDistSq = distSq;
        }
    }

    return nearest;
}
