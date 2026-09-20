/*
 * mod-party-bridge - snapshot builder (WORLD THREAD ONLY).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * Emits the `Snapshot` type from the wow-party PROTOCOL.md v1. Money is copper, ts is unix seconds.
 * The master's ACTIVE character is always included (`self`) because everything on the dashboard is
 * meant to be read against it: every bot carries a quest comparison with the master's quest log.
 */

#include "PartyBridge.h"

#include "AiFactory.h"
#include "AiObjectContext.h"
#include "Bag.h"
#include "DBCStores.h"
#include "Engine.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "StringFormat.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    // The Engine pointer of a PlayerbotAI is protected and the fork offers no accessor. Naming the
    // member through a derived type is the standard-conforming way to take its address without
    // patching (and later re-merging) the fork. Never instantiated.
    struct CurrentEngineAccess : PlayerbotAI
    {
        static Engine* PlayerbotAI::* Ptr() { return &CurrentEngineAccess::currentEngine; }
    };

    char const* const kClassNames[] = { "", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight",
                                        "Shaman", "Mage", "Warlock", "", "Druid" };
    char const* const kRaceNames[] = { "", "Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome",
                                       "Troll", "", "Blood Elf", "Draenei" };
    char const* const kQualityColor[] = { "9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000", "e6cc80", "e6cc80" };

    template <size_t N>
    char const* NameOf(char const* const (&table)[N], uint32 id)
    {
        return id < N ? table[id] : "";
    }

    std::string AreaName(uint32 areaId)
    {
        if (AreaTableEntry const* a = sAreaTableStore.LookupEntry(areaId))
            return a->area_name[0] ? a->area_name[0] : "";
        return {};
    }

    std::string Lower(char const* s)
    {
        std::string out = s ? s : "";
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
        return out;
    }

    std::string ItemLink(ItemTemplate const* proto)
    {
        uint32 const q = std::min<uint32>(proto->Quality, 7);
        return Acore::StringFormat("|cff{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", kQualityColor[q], proto->ItemId, proto->Name1);
    }

    json ItemJson(Item* item, uint8 bag, uint8 slot)
    {
        ItemTemplate const* proto = item->GetTemplate();
        json j;
        j["entry"] = proto->ItemId;
        j["name"] = proto->Name1;
        j["quality"] = std::min<uint32>(proto->Quality, 7);
        j["itemLevel"] = proto->ItemLevel;
        j["requiredLevel"] = proto->RequiredLevel;
        j["count"] = item->GetCount();

        json icon = nullptr;
        if (ItemDisplayInfoEntry const* di = sItemDisplayInfoStore.LookupEntry(proto->DisplayInfoID))
            if (di->inventoryIcon && *di->inventoryIcon)
                icon = Lower(di->inventoryIcon);
        j["icon"] = icon;

        j["slot"] = slot;
        j["bag"] = bag;

        uint32 const maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (maxDur > 0)
            j["durability"] = { { "cur", item->GetUInt32Value(ITEM_FIELD_DURABILITY) }, { "max", maxDur } };

        j["sellPrice"] = proto->SellPrice;
        j["soulbound"] = item->IsSoulBound();
        j["tradeable"] = !item->IsSoulBound();
        j["link"] = ItemLink(proto);
        return j;
    }

    json StatsJson(Player* p)
    {
        float spellCrit = 0.0f;
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            spellCrit = std::max(spellCrit, p->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));

        json j;
        j["str"] = static_cast<int>(p->GetStat(STAT_STRENGTH));
        j["agi"] = static_cast<int>(p->GetStat(STAT_AGILITY));
        j["sta"] = static_cast<int>(p->GetStat(STAT_STAMINA));
        j["int"] = static_cast<int>(p->GetStat(STAT_INTELLECT));
        j["spi"] = static_cast<int>(p->GetStat(STAT_SPIRIT));
        j["armor"] = p->GetArmor();
        j["attackPower"] = static_cast<int>(p->GetTotalAttackPowerValue(BASE_ATTACK));
        j["rangedAttackPower"] = static_cast<int>(p->GetTotalAttackPowerValue(RANGED_ATTACK));
        j["spellPower"] = p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC);
        j["healingPower"] = p->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_MAGIC);
        j["critPct"] = p->GetFloatValue(PLAYER_CRIT_PERCENTAGE);
        j["spellCritPct"] = spellCrit;
        j["rangedCritPct"] = p->GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE);
        j["hitPct"] = p->GetRatingBonusValue(CR_HIT_MELEE);
        j["spellHitPct"] = p->GetRatingBonusValue(CR_HIT_SPELL);
        j["hastePct"] = p->GetRatingBonusValue(CR_HASTE_MELEE);
        j["dodgePct"] = p->GetFloatValue(PLAYER_DODGE_PERCENTAGE);
        j["parryPct"] = p->GetFloatValue(PLAYER_PARRY_PERCENTAGE);
        j["blockPct"] = p->GetFloatValue(PLAYER_BLOCK_PERCENTAGE);
        j["resist"] = {
            { "holy", p->GetResistance(SPELL_SCHOOL_HOLY) },
            { "fire", p->GetResistance(SPELL_SCHOOL_FIRE) },
            { "nature", p->GetResistance(SPELL_SCHOOL_NATURE) },
            { "frost", p->GetResistance(SPELL_SCHOOL_FROST) },
            { "shadow", p->GetResistance(SPELL_SCHOOL_SHADOW) },
            { "arcane", p->GetResistance(SPELL_SCHOOL_ARCANE) }
        };
        // Regen while casting (interrupted), per second -> per 5 seconds.
        j["mp5"] = static_cast<int>(p->GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) * 5.0f);
        return j;
    }

    // ---- quests ----

    // Objective progress of quest `q` for player `p`; slot = that player's quest log slot.
    json ObjectivesJson(Player* p, Quest const* q, uint16 slot)
    {
        json objectives = json::array();

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const target = q->RequiredNpcOrGo[i];
            uint32 const req = q->RequiredNpcOrGoCount[i];
            if (!target || !req)
                continue;

            std::string text = q->ObjectiveText[i];
            if (text.empty())
            {
                if (target > 0)
                {
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(static_cast<uint32>(target)))
                        text = ct->Name;
                }
                else if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(static_cast<uint32>(-target)))
                    text = go->name;
            }
            objectives.push_back({ { "text", text }, { "cur", p->GetQuestSlotCounter(slot, i) }, { "req", req } });
        }

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const itemId = q->RequiredItemId[i];
            uint32 const req = q->RequiredItemCount[i];
            if (!itemId || !req)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            objectives.push_back({ { "text", proto ? proto->Name1 : std::string("item") },
                                   { "cur", p->GetItemCount(itemId, false) }, { "req", req } });
        }
        return objectives;
    }

    std::string QuestStateName(Player* p, uint32 questId)
    {
        if (p->IsQuestRewarded(questId))
            return "rewarded";
        switch (p->GetQuestStatus(questId))
        {
            case QUEST_STATUS_COMPLETE: return "complete";
            case QUEST_STATUS_FAILED: return "failed";
            case QUEST_STATUS_INCOMPLETE: return "incomplete";
            case QUEST_STATUS_REWARDED: return "rewarded";
            default: return "none";
        }
    }

    json QuestsJson(Player* p)
    {
        json arr = json::array();
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = p->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            Quest const* q = sObjectMgr->GetQuestTemplate(questId);
            if (!q)
                continue;

            std::string const state = QuestStateName(p, questId);
            arr.push_back({
                { "id", questId },
                { "title", q->GetTitle() },
                { "level", q->GetQuestLevel() },
                { "status", (state == "complete" || state == "failed") ? state : std::string("incomplete") },
                { "objectives", ObjectivesJson(p, q, slot) },
                { "shareable", q->HasFlag(QUEST_FLAGS_SHARABLE) }
            });
        }
        return arr;
    }

    // Why `bot` cannot take `q` right now (first matching reason), or null if it could / nothing to do.
    // All Satisfy* checks run with msg=false, so nothing is sent to any client.
    json BlockerFor(Player* bot, Quest const* q)
    {
        std::string const state = QuestStateName(bot, q->GetQuestId());
        if (state == "rewarded")
            return nullptr;   // already handed in: nothing to fix
        if (state != "none")
            return "has_it";

        if (!bot->SatisfyQuestClass(q, false)) return "class";
        if (!bot->SatisfyQuestRace(q, false)) return "race";
        if (!bot->SatisfyQuestSkill(q, false)) return "skill";
        if (!bot->SatisfyQuestLevel(q, false)) return "level";
        if (!bot->SatisfyQuestReputation(q, false)) return "reputation";
        if (!bot->SatisfyQuestPreviousQuest(q, false)) return "prerequisite";
        if (!bot->SatisfyQuestExclusiveGroup(q, false)) return "exclusive";
        if (!bot->SatisfyQuestNextChain(q, false) || !bot->SatisfyQuestPrevChain(q, false) ||
            !bot->SatisfyQuestBreadcrumb(q, false))
            return "chain";
        if (!bot->SatisfyQuestDay(q, false) || !bot->SatisfyQuestWeek(q, false) ||
            !bot->SatisfyQuestMonth(q, false) || !bot->SatisfyQuestSeasonal(q, false))
            return "repeat";
        if (!bot->SatisfyQuestConditions(q, false)) return "conditions";
        if (!bot->SatisfyQuestLog(false)) return "log_full";
        return nullptr;
    }

    bool SameObjectives(json const& a, json const& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i]["cur"] != b[i]["cur"] || a[i]["req"] != b[i]["req"])
                return false;
        return true;
    }

    // Bot vs. the master's active character, keyed on quest log contents.
    json QuestCompareJson(Player* master, Player* bot)
    {
        json missingOnBot = json::array(), missingOnMaster = json::array(), differing = json::array();
        uint32 inSync = 0;

        // An out-of-range slot (MAX_QUEST_LOG_SIZE) means "not in that character's log".
        auto entry = [&](Quest const* q, uint16 mSlot, uint16 bSlot, bool withBlocker)
        {
            uint32 const id = q->GetQuestId();
            json e;
            e["id"] = id;
            e["title"] = q->GetTitle();
            e["level"] = q->GetQuestLevel();
            e["master"] = { { "state", QuestStateName(master, id) },
                            { "objectives", mSlot < MAX_QUEST_LOG_SIZE ? ObjectivesJson(master, q, mSlot) : json::array() } };
            e["bot"] = { { "state", QuestStateName(bot, id) },
                         { "objectives", bSlot < MAX_QUEST_LOG_SIZE ? ObjectivesJson(bot, q, bSlot) : json::array() },
                         { "blocker", withBlocker ? BlockerFor(bot, q) : json(nullptr) } };
            return e;
        };

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const id = master->GetQuestSlotQuestId(slot);
            Quest const* q = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
            if (!q)
                continue;

            uint16 const bSlot = bot->FindQuestSlot(id);
            if (bSlot >= MAX_QUEST_LOG_SIZE)
            {
                missingOnBot.push_back(entry(q, slot, MAX_QUEST_LOG_SIZE, true));
                continue;
            }

            json e = entry(q, slot, bSlot, false);
            if (e["master"]["state"] == e["bot"]["state"] && SameObjectives(e["master"]["objectives"], e["bot"]["objectives"]))
                ++inSync;
            else
                differing.push_back(std::move(e));
        }

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const id = bot->GetQuestSlotQuestId(slot);
            Quest const* q = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
            if (q && master->FindQuestSlot(id) >= MAX_QUEST_LOG_SIZE)
                missingOnMaster.push_back(entry(q, MAX_QUEST_LOG_SIZE, slot, false));
        }

        json out;
        out["missingOnBot"] = missingOnBot;
        out["missingOnMaster"] = missingOnMaster;
        out["differing"] = differing;
        out["inSync"] = inSync;
        out["botLogFull"] = bot->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE;
        return out;
    }

    json PosJson(Player* p)
    {
        return {
            { "map", p->GetMapId() },
            { "zone", p->GetZoneId() }, { "zoneName", AreaName(p->GetZoneId()) },
            { "area", p->GetAreaId() }, { "areaName", AreaName(p->GetAreaId()) },
            { "x", p->GetPositionX() }, { "y", p->GetPositionY() }, { "z", p->GetPositionZ() }, { "o", p->GetOrientation() }
        };
    }

    // The fields a bot and the master's active character have in common (PROTOCOL.md `Character`).
    // `scoreAi` is only used for GetEquipGearScore, which needs some PlayerbotAI instance.
    json CharacterJson(Player* p, PlayerbotAI* scoreAi)
    {
        json m;
        m["guid"] = p->GetGUID().GetCounter();
        m["name"] = p->GetName();
        m["race"] = p->getRace();
        m["raceName"] = NameOf(kRaceNames, p->getRace());
        m["cls"] = p->getClass();
        m["clsName"] = NameOf(kClassNames, p->getClass());
        std::string const spec = AiFactory::GetPlayerSpecName(p);
        m["spec"] = spec.empty() ? json(nullptr) : json(spec);
        m["level"] = p->GetLevel();
        m["xp"] = p->GetUInt32Value(PLAYER_XP);
        m["xpMax"] = p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        m["hp"] = p->GetHealth();
        m["hpMax"] = p->GetMaxHealth();

        Powers const pt = p->getPowerType();
        m["powerType"] = static_cast<int>(pt);
        // Rage and runic power are stored x10 internally; report what the client shows.
        uint32 const powerDiv = (pt == POWER_RAGE || pt == POWER_RUNIC_POWER) ? 10 : 1;
        m["power"] = p->GetPower(pt) / powerDiv;
        m["powerMax"] = p->GetMaxPower(pt) / powerDiv;
        m["gold"] = p->GetMoney();
        m["gearScore"] = scoreAi ? scoreAi->GetEquipGearScore(p) : 0;

        // Equipment + averages.
        json equipment = json::array();
        uint32 ilvlSum = 0, ilvlCount = 0;
        float durSum = 0.0f;
        uint32 durCount = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            equipment.push_back(ItemJson(item, INVENTORY_SLOT_BAG_0, slot));

            ItemTemplate const* proto = item->GetTemplate();
            if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD)
            {
                ilvlSum += proto->ItemLevel;
                ++ilvlCount;
            }
            uint32 const maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maxDur > 0)
            {
                durSum += 100.0f * item->GetUInt32Value(ITEM_FIELD_DURABILITY) / maxDur;
                ++durCount;
            }
        }
        m["equipment"] = equipment;
        m["avgItemLevel"] = ilvlCount ? static_cast<double>(ilvlSum) / ilvlCount : 0.0;
        m["durabilityPct"] = durCount ? durSum / durCount : 100.0f;

        // Bags: backpack is bag 0, equipped bags are 1..4.
        json bags = json::array();
        json bagSlots = json::array();
        bagSlots.push_back({ { "bag", 0 }, { "entry", 0 }, { "name", "Rucksack" }, { "quality", 1 },
                             { "icon", "inv_misc_bag_08" }, { "size", 16 } });
        uint32 totalSlots = 16;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bags.push_back(ItemJson(item, 0, slot - INVENTORY_SLOT_ITEM_START));

        for (uint8 b = 0; b < INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START; ++b)
        {
            Bag* bag = p->GetBagByPos(INVENTORY_SLOT_BAG_START + b);
            if (!bag)
                continue;
            totalSlots += bag->GetBagSize();
            {
                json info = ItemJson(bag, 0, 0);
                bagSlots.push_back({ { "bag", b + 1 }, { "entry", info["entry"] }, { "name", info["name"] },
                                     { "quality", info["quality"] }, { "icon", info["icon"] },
                                     { "size", bag->GetBagSize() } });
            }
            for (uint32 s = 0; s < bag->GetBagSize(); ++s)
                if (Item* item = bag->GetItemByPos(static_cast<uint8>(s)))
                    bags.push_back(ItemJson(item, b + 1, static_cast<uint8>(s)));
        }
        m["bags"] = bags;
        m["bagSlots"] = bagSlots;
        m["freeSlots"] = p->GetFreeInventorySpace();
        m["totalSlots"] = totalSlots;

        m["pos"] = PosJson(p);
        m["stats"] = StatsJson(p);
        m["quests"] = QuestsJson(p);
        return m;
    }

    json MemberJson(Player* master, Player* bot, PlayerbotAI* ai)
    {
        json m = CharacterJson(bot, ai);
        m["isBot"] = true;

        // State / what the bot is doing.
        char const* state = "unknown";
        switch (ai->GetState())
        {
            case BOT_STATE_COMBAT: state = "combat"; break;
            case BOT_STATE_NON_COMBAT: state = "non-combat"; break;
            case BOT_STATE_DEAD: state = "dead"; break;
            default: break;
        }
        m["state"] = state;

        // Engine::lastAction is a history string "|action|action|..." (max ~512 chars), newest last.
        json recent = json::array();
        std::string last;
        if (Engine* engine = ai->*CurrentEngineAccess::Ptr())
        {
            std::string const history = engine->GetLastAction();
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= history.size())
            {
                size_t const end = history.find('|', start);
                std::string part = history.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!part.empty())
                    parts.push_back(std::move(part));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            size_t const from = parts.size() > 6 ? parts.size() - 6 : 0;
            for (size_t i = from; i < parts.size(); ++i)
                recent.push_back(parts[i]);
            if (!parts.empty())
                last = parts.back();
        }
        m["lastAction"] = last;
        m["recentActions"] = recent;

        // Playerbots has no persistent plan: its queue is filled and drained inside one AI tick.
        // "Next" is therefore a heuristic from what is observable right now.
        json queued = json::array();
        Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (target && target->IsAlive())
        {
            m["target"] = { { "name", target->GetName() }, { "hpPct", target->GetHealthPct() } };
            if (ai->GetState() == BOT_STATE_COMBAT)
                queued.push_back("Angriff auf " + target->GetName());
        }
        else
            m["target"] = nullptr;
        m["queuedActions"] = queued;

        std::vector<std::string> const co = ai->GetStrategies(BOT_STATE_COMBAT);
        std::vector<std::string> const nc = ai->GetStrategies(BOT_STATE_NON_COMBAT);
        m["strategies"] = { { "co", co }, { "nc", nc } };
        m["following"] = std::find(nc.begin(), nc.end(), "follow") != nc.end();

        m["distanceToMaster"] = bot->GetMapId() == master->GetMapId() ? json(bot->GetDistance(master)) : json(nullptr);
        m["pendingQuestOffers"] = json::array();   // Phase 2 (approval gate for shared quests)
        m["questCompare"] = QuestCompareJson(master, bot);
        return m;
    }
}

namespace PartyBridge
{
    bool IsBot(Player* player)
    {
        return player && PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
    }

    std::vector<Player*> CollectBots(Player* master)
    {
        std::vector<Player*> bots;
        Group* group = master ? master->GetGroup() : nullptr;
        if (!group)
            return bots;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != master && member->IsInWorld() && IsBot(member))
                bots.push_back(member);
        }
        return bots;
    }

    std::string BuildSnapshot(Player* master)
    {
        std::vector<Player*> const bots = CollectBots(master);
        if (bots.empty())
            return {};

        uint64 goldTotal = master->GetMoney();
        json members = json::array();
        PlayerbotAI* scoreAi = nullptr;
        for (Player* bot : bots)
        {
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (!ai)
                continue;
            if (!scoreAi)
                scoreAi = ai;
            goldTotal += bot->GetMoney();
            members.push_back(MemberJson(master, bot, ai));
        }

        json snap;
        snap["v"] = 1;
        snap["ts"] = static_cast<uint64>(GameTime::GetGameTime().count());
        snap["master"] = {
            { "accountId", master->GetSession()->GetAccountId() },
            { "guid", master->GetGUID().GetCounter() },
            { "name", master->GetName() },
            { "level", master->GetLevel() },
            { "gold", master->GetMoney() },
            { "map", master->GetMapId() },
            { "zone", master->GetZoneId() }, { "zoneName", AreaName(master->GetZoneId()) },
            { "areaName", AreaName(master->GetAreaId()) }
        };
        snap["goldTotal"] = goldTotal;
        snap["self"] = CharacterJson(master, scoreAi);
        snap["members"] = members;

        // Item/quest names can contain bytes that are not valid UTF-8 -> replace instead of throwing.
        return snap.dump(-1, ' ', false, json::error_handler_t::replace);
    }
}
