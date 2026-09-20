/*
 * mod-party-bridge - dashboard command executor (WORLD THREAD ONLY).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * A command is never executed by poking the bot directly. It is translated into the exact chat
 * command the master could have whispered ("accept [quest]", "t [item]", "co +dps", ...) and fed
 * through PlayerbotAI::HandleCommand, so Playerbots' own security checks and action code decide
 * whether it runs. "ok" in a CommandResult therefore means "delivered to the bot", not "done":
 * bots execute chat commands on their next AI tick.
 */

#include "PartyBridge.h"

#include "GameTime.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "StringFormat.h"

#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>

using json = nlohmann::json;

namespace
{
    constexpr size_t kMaxChatLen = 200;
    constexpr size_t kMaxTradeItems = 6;   // the trade window has 6 tradable slots

    // op -> exact playerbots chat command, for ops without arguments.
    std::map<std::string, std::string> const kSimpleOps = {
        { "follow", "follow" }, { "stay", "stay" }, { "flee", "flee" }, { "grind", "grind" },
        { "attack", "attack" }, { "summon", "summon" }, { "release", "release" }, { "revive", "revive" },
        { "repair", "repair" }, { "maintenance", "maintenance" }, { "loot-all", "loot all" }
    };

    // op -> chat command prefix, for ops taking one item entry.
    std::map<std::string, std::string> const kItemOps = {
        { "item.equip", "e" }, { "item.unequip", "ue" }, { "item.use", "u" },
        { "item.sell", "s" }, { "item.destroy", "destroy" }
    };

    void Reply(std::string const& id, std::string const& bot, std::string const& op, bool ok, std::string const& message)
    {
        json j = {
            { "id", id }, { "ok", ok }, { "message", message }, { "bot", bot }, { "op", op },
            { "ts", static_cast<uint64>(GameTime::GetGameTime().count()) }
        };
        PartyBridge::Bridge::instance().Publish(PartyBridge::Channel::Result, 0,
                                                j.dump(-1, ' ', false, json::error_handler_t::replace));
    }

    // Printable, single-line, no dot-prefix (would be a GM command), bounded length.
    bool IsSafeText(std::string const& s, size_t maxLen)
    {
        if (s.empty() || s.size() > maxLen || s.front() == '.')
            return false;
        for (unsigned char c : s)
            if (c < 0x20 || c == 0x7f)
                return false;
        return true;
    }

    std::optional<std::string> QuestLink(uint32 questId)
    {
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q)
            return std::nullopt;
        return Acore::StringFormat("|cffffff00|Hquest:{}:{}|h[{}]|h|r", questId, q->GetQuestLevel(), q->GetTitle());
    }

    std::optional<std::string> ItemLink(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return std::nullopt;
        return Acore::StringFormat("|cffffffff|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", entry, proto->Name1);
    }

    // Translates (op, args) into the playerbots chat command. Returns an error text on failure.
    std::optional<std::string> BuildText(std::string const& op, json const& args, std::string& text)
    {
        if (auto it = kSimpleOps.find(op); it != kSimpleOps.end())
        {
            text = it->second;
            return std::nullopt;
        }

        if (op == "chat")
        {
            text = args.value("text", std::string());
            return IsSafeText(text, kMaxChatLen) ? std::nullopt : std::optional<std::string>("ungueltiger Befehlstext");
        }

        if (op == "strategy")
        {
            std::string const scope = args.value("scope", std::string());
            std::string const changes = args.value("changes", std::string());
            if ((scope != "co" && scope != "nc") || changes.empty() || changes.size() > 120)
                return "ungueltige Strategie-Angabe";
            for (unsigned char c : changes)
                if (!std::isalnum(c) && c != ' ' && c != '+' && c != '-' && c != ',' && c != '_')
                    return "ungueltige Zeichen in der Strategie-Angabe";
            text = scope + " " + changes;
            return std::nullopt;
        }

        if (op == "quest.accept" || op == "quest.abandon" || op == "quest.share")
        {
            auto link = QuestLink(args.value("questId", 0u));
            if (!link)
                return "unbekannte Quest";
            text = (op == "quest.accept" ? "accept " : op == "quest.abandon" ? "drop " : "share ") + *link;
            return std::nullopt;
        }

        if (auto it = kItemOps.find(op); it != kItemOps.end())
        {
            auto link = ItemLink(args.value("entry", 0u));
            if (!link)
                return "unbekanntes Item";
            text = it->second + " " + *link;
            return std::nullopt;
        }

        if (op == "item.trade")
        {
            json const& items = args.contains("items") ? args["items"] : json::array();
            if (!items.is_array() || items.empty() || items.size() > kMaxTradeItems)
                return "item.trade braucht 1-" + std::to_string(kMaxTradeItems) + " Items";
            text = "t";
            for (json const& it : items)
            {
                auto link = ItemLink(it.value("entry", 0u));
                if (!link)
                    return "unbekanntes Item";
                text += " " + *link;
            }
            return std::nullopt;
        }

        if (op == "quest.offer.accept" || op == "quest.offer.decline")
            return "Quest-Angebote sind noch nicht implementiert (Phase 2)";

        return "unbekannte Aktion '" + op + "'";
    }

    // The master a bot obeys: a real player in the same group, allowed by config.
    Player* MasterOf(Player* bot)
    {
        PlayerbotAI* ai = PartyBridge::IsBot(bot) ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        Player* master = ai ? ai->GetMaster() : nullptr;
        if (!master || !master->IsInWorld() || PartyBridge::IsBot(master))
            return nullptr;
        if (!master->GetGroup() || master->GetGroup() != bot->GetGroup())
            return nullptr;
        if (!PartyBridge::Bridge::instance().IsMasterAllowed(master->GetSession()->GetAccountId()))
            return nullptr;
        return master;
    }

    // For bot "*": the first allowed real player that has bots in the group.
    Player* FirstMasterWithBots()
    {
        for (auto const& kv : ObjectAccessor::GetPlayers())
        {
            Player* p = kv.second;
            if (p && p->IsInWorld() && !PartyBridge::IsBot(p) &&
                PartyBridge::Bridge::instance().IsMasterAllowed(p->GetSession()->GetAccountId()) &&
                !PartyBridge::CollectBots(p).empty())
                return p;
        }
        return nullptr;
    }

    void Deliver(Player* master, Player* bot, std::string const& id, std::string const& op, std::string const& text)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai)
        {
            Reply(id, bot->GetName(), op, false, "kein Playerbot");
            return;
        }
        LOG_DEBUG("module", "[mod-party-bridge] {} <- '{}'", bot->GetName(), text);
        ai->HandleCommand(CHAT_MSG_WHISPER, text, master);
        Reply(id, bot->GetName(), op, true, "gesendet: " + text);
    }
}

namespace PartyBridge
{
    void ExecuteCommand(std::string const& raw)
    {
        std::string id;
        std::string botName;
        std::string op;
        try
        {
            json cmd = json::parse(raw);
            id = cmd.value("id", std::string());
            botName = cmd.value("bot", std::string());
            op = cmd.value("op", std::string());
            json const args = cmd.contains("args") && cmd["args"].is_object() ? cmd["args"] : json::object();

            if (botName.empty() || op.empty())
            {
                Reply(id, botName, op, false, "bot und op sind Pflicht");
                return;
            }

            std::string text;
            if (std::optional<std::string> err = BuildText(op, args, text))
            {
                Reply(id, botName, op, false, *err);
                return;
            }

            if (botName == "*")
            {
                Player* master = FirstMasterWithBots();
                if (!master)
                {
                    Reply(id, botName, op, false, "kein Master mit Bots in der Gruppe online");
                    return;
                }
                for (Player* bot : CollectBots(master))
                    Deliver(master, bot, id, op, text);
                return;
            }

            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            if (!bot || !IsBot(bot))
            {
                Reply(id, botName, op, false, "Bot nicht online");
                return;
            }
            Player* master = MasterOf(bot);
            if (!master)
            {
                Reply(id, botName, op, false, "Bot ist nicht in der Gruppe eines erlaubten Masters");
                return;
            }
            Deliver(master, bot, id, op, text);
        }
        catch (std::exception const& e)
        {
            // Malformed JSON or wrong argument types must never reach the world loop as an exception.
            Reply(id, botName, op, false, std::string("ungueltiger Befehl: ") + e.what());
        }
    }
}
