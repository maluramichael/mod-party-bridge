/*
 * mod-party-bridge - WorldScript hooks.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * The only place where the world thread and the Redis worker threads meet: every update tick it
 * drains a bounded number of queued dashboard commands, and every SnapshotIntervalMs it publishes
 * one snapshot per master. Both are cheap no-ops while the bridge is inactive.
 */

#include "PartyBridge.h"

#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <vector>

class PartyBridgeWorldScript : public WorldScript
{
public:
    PartyBridgeWorldScript() : WorldScript("PartyBridgeWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        PartyBridge::Bridge::instance().LoadConfig();
    }

    void OnStartup() override
    {
        PartyBridge::Bridge::instance().LoadConfig();
        PartyBridge::Bridge::instance().Start();
    }

    void OnShutdown() override
    {
        PartyBridge::Bridge::instance().Stop();
    }

    void OnUpdate(uint32 diff) override
    {
        PartyBridge::Bridge& bridge = PartyBridge::Bridge::instance();
        if (!bridge.IsActive())
            return;

        // 1) Dashboard commands (bounded per tick so a flood cannot stall the world).
        std::vector<std::string> commands;
        bridge.DrainCommands(commands, bridge.MaxCommandsPerTick());
        for (std::string const& raw : commands)
            PartyBridge::ExecuteCommand(raw);

        // 2) Snapshots.
        _accum += diff;
        if (_accum < bridge.SnapshotIntervalMs())
            return;
        _accum = 0;

        for (auto const& kv : ObjectAccessor::GetPlayers())
        {
            Player* p = kv.second;
            if (!p || !p->IsInWorld() || PartyBridge::IsBot(p) || !p->GetSession())
                continue;

            uint32 const accountId = p->GetSession()->GetAccountId();
            if (!bridge.IsMasterAllowed(accountId))
                continue;

            std::string snapshot = PartyBridge::BuildSnapshot(p);
            if (!snapshot.empty())
                bridge.Publish(PartyBridge::Channel::Snapshot, accountId, std::move(snapshot));
        }
    }

private:
    uint32 _accum = 0;
};

void AddPartyBridgeScripts()
{
    new PartyBridgeWorldScript();
}
