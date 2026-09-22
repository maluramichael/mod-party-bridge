/*
 * mod-party-bridge - PlayerScript hooks that publish live PartyEvents (WORLD THREAD ONLY).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * Emits the `PartyEvent` type from wow-party PROTOCOL.md v1 on Redis channel `wowparty:evt`
 * (PartyBridge::Channel::Event) for bot-side happenings a dashboard viewer cares about between
 * snapshots: death, level-up, quest status changes, and trade open. Relevance mirrors
 * PartySnapshot.cpp / PartyCommands.cpp: the event only goes out if the acting character is a
 * Playerbot whose master is a real, grouped, config-allowed player (PartyBridge::MasterOf).
 * Building JSON and publishing here is cheap (Bridge::Publish only queues a string for the
 * background publisher thread); nothing here ever touches a socket on the world thread.
 */

#include "PartyBridge.h"

#include "GameTime.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    // Publishes one PartyEvent for `bot` (whose allowed master is `master`). `data` may be empty.
    void EmitEvent(Player* master, Player* bot, char const* type, std::string const& text, json data = json::object())
    {
        json j;
        j["type"] = type;
        j["bot"] = bot->GetName();
        j["text"] = text;
        j["ts"] = static_cast<uint64>(GameTime::GetGameTime().count());
        if (!data.empty())
            j["data"] = std::move(data);

        PartyBridge::Bridge::instance().Publish(PartyBridge::Channel::Event, master->GetSession()->GetAccountId(),
                                                j.dump(-1, ' ', false, json::error_handler_t::replace));
    }
}

class PartyEventPlayerScript : public PlayerScript
{
public:
    // This fork only calls a PlayerScript hook if the script explicitly opted in here
    // (ScriptMgr::EnabledHooks) - an unlisted override is silently never invoked.
    PartyEventPlayerScript() : PlayerScript("PartyEventPlayerScript",
        { PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
          PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_QUEST_ABANDON, PLAYERHOOK_CAN_INIT_TRADE })
    {
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!PartyBridge::Bridge::instance().IsActive())
            return;

        Player* master = PartyBridge::MasterOf(player);
        if (!master)
            return;

        EmitEvent(master, player, "bot_death", player->GetName() + " ist gestorben.",
                  { { "level", player->GetLevel() } });
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!PartyBridge::Bridge::instance().IsActive())
            return;

        Player* master = PartyBridge::MasterOf(player);
        if (!master)
            return;

        EmitEvent(master, player, "bot_levelup",
                  player->GetName() + " ist jetzt Level " + std::to_string(player->GetLevel()) + ".",
                  { { "oldLevel", oldLevel }, { "newLevel", player->GetLevel() } });
    }

    // Quest "status change" covers all three transitions Playerbots can reach on its own:
    // accepted, turned in (rewarded), and abandoned. All three map to PROTOCOL.md's "quest_update".
    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        EmitQuestUpdate(player, quest, "accepted", "hat die Quest angenommen: ");
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        EmitQuestUpdate(player, quest, "completed", "hat die Quest abgeschlossen: ");
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (!PartyBridge::Bridge::instance().IsActive())
            return;

        Player* master = PartyBridge::MasterOf(player);
        if (!master)
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::string const title = quest ? quest->GetTitle() : std::string();
        EmitEvent(master, player, "quest_update", player->GetName() + " hat die Quest abgebrochen: " + title,
                  { { "questId", questId }, { "title", title }, { "action", "abandoned" } });
    }

    // Fires right before a trade window opens (after all of Player.cpp's own distance/faction/etc.
    // checks pass); never blocks it. `player` initiated CMSG_INITIATE_TRADE, `target` is the other side.
    // Either direction can be the bot (Playerbots' "t <item>" whisper makes the BOT open the trade).
    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (!PartyBridge::Bridge::instance().IsActive())
            return true;

        if (Player* master = PartyBridge::MasterOf(player); master == target)
            EmitEvent(master, player, "trade_opened",
                      player->GetName() + " hat einen Handel mit " + target->GetName() + " eroeffnet.",
                      { { "with", target->GetName() } });
        else if (Player* master2 = PartyBridge::MasterOf(target); master2 == player)
            EmitEvent(master2, target, "trade_opened",
                      target->GetName() + " hat einen Handel mit " + player->GetName() + " eroeffnet.",
                      { { "with", player->GetName() } });

        return true;
    }

private:
    // `textPrefix` is the German verb phrase ("hat die Quest angenommen: ", ...); the quest title is
    // appended after the null check below, never dereferenced before it.
    void EmitQuestUpdate(Player* player, Quest const* quest, char const* action, char const* textPrefix)
    {
        if (!quest || !PartyBridge::Bridge::instance().IsActive())
            return;

        Player* master = PartyBridge::MasterOf(player);
        if (!master)
            return;

        std::string const title = quest->GetTitle();
        EmitEvent(master, player, "quest_update", player->GetName() + " " + textPrefix + title,
                  { { "questId", quest->GetQuestId() }, { "title", title }, { "action", action } });
    }
};

void AddPartyEventScripts()
{
    new PartyEventPlayerScript();
}
