/*
 * mod-party-bridge - live Playerbots party dashboard bridge.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * Threading contract:
 *  - Everything that touches Player/Item/Quest/PlayerbotAI runs on the WORLD thread only
 *    (snapshot building in PartySnapshot.cpp, command execution in PartyCommands.cpp).
 *  - Redis I/O lives on two background threads owned by Bridge (publisher + command puller).
 *    They exchange plain strings with the world thread through mutex-guarded bounded queues.
 *  - A dead Redis can therefore never stall or crash the world: data is dropped, connects
 *    are retried with backoff.
 */

#ifndef MOD_PARTY_BRIDGE_H
#define MOD_PARTY_BRIDGE_H

#include "Define.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class Player;

namespace PartyBridge
{
    enum class Channel : uint8
    {
        Snapshot,   // PUBLISH wowparty:snap + SET wowparty:last:<accountId>
        Event,      // PUBLISH wowparty:evt
        Result      // PUBLISH wowparty:res
    };

    class Bridge
    {
    public:
        static Bridge& instance();

        // Reads PartyBridge.* from sConfigMgr. Safe to call repeatedly (OnAfterConfigLoad).
        void LoadConfig();
        void Start();
        void Stop();

        bool IsActive() const { return _active.load(std::memory_order_relaxed); }

        // World thread -> Redis publisher. Non-blocking, never throws.
        void Publish(Channel channel, uint32 accountId, std::string payload);

        // Redis command puller -> world thread. Moves up to `max` raw command JSON strings out.
        void DrainCommands(std::vector<std::string>& out, size_t max);

        uint32 SnapshotIntervalMs() const { return _snapshotIntervalMs; }
        uint32 MaxCommandsPerTick() const { return _maxCommandsPerTick; }
        bool IsMasterAllowed(uint32 accountId) const;

    private:
        Bridge() = default;
        ~Bridge();
        Bridge(Bridge const&) = delete;
        Bridge& operator=(Bridge const&) = delete;

        void PublisherMain();
        void PullerMain();

        struct Out
        {
            Channel channel;
            uint32 accountId;
            std::string payload;
        };

        // --- config ---
        bool _enabledCfg = false;
        std::string _host = "127.0.0.1";
        int _port = 6379;
        uint32 _snapshotIntervalMs = 1000;
        uint32 _maxCommandsPerTick = 8;
        std::set<uint32> _masterAccounts;   // empty = every real player

        // --- runtime ---
        std::atomic<bool> _active{false};
        std::atomic<bool> _running{false};
        std::thread _publisher;
        std::thread _puller;

        std::mutex _outMutex;
        std::condition_variable _outCv;
        std::deque<Out> _out;
        uint64 _dropped = 0;

        std::mutex _inMutex;
        std::deque<std::string> _in;
    };

    // ---- PartySnapshot.cpp (world thread) ----

    // One snapshot JSON for `master` and the bots in its group. Empty string if there are no bots.
    std::string BuildSnapshot(Player* master);

    // Bots of master's group (bots only, master excluded).
    std::vector<Player*> CollectBots(Player* master);

    bool IsBot(Player* player);

    // ---- PartyCommands.cpp (world thread) ----

    // Parses and executes one raw command JSON. Always publishes exactly one CommandResult per
    // targeted bot (or a single failure result if the command itself is unusable).
    void ExecuteCommand(std::string const& raw);
}

#endif // MOD_PARTY_BRIDGE_H
