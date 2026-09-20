/*
 * mod-party-bridge - Redis worker threads.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, or (at your
 * option) any later version.
 *
 * Two threads, one hiredis context each (a context is not thread-safe, and BRPOP blocks):
 *   publisher - drains the outgoing queue: PUBLISH snapshots/events/results, SET the last snapshot
 *   puller    - BRPOP wowparty:cmd (1 s timeout) and hands raw command JSON to the world thread
 */

#include "PartyBridge.h"

#include "Config.h"
#include "Log.h"
#include "StringConvert.h"
#include "Tokenize.h"

#include <algorithm>
#include <chrono>

#ifdef PARTY_BRIDGE_AVAILABLE
#include <hiredis/hiredis.h>
#endif

namespace
{
    constexpr size_t kMaxOutQueue = 512;     // snapshots are ~10-100 KB; older ones are worthless
    constexpr size_t kMaxInQueue = 1024;
    constexpr size_t kMaxCommandBytes = 8192;
    constexpr uint32 kBackoffMinMs = 1000;
    constexpr uint32 kBackoffMaxMs = 30000;

    char const* const kChanSnap = "wowparty:snap";
    char const* const kChanEvt = "wowparty:evt";
    char const* const kChanRes = "wowparty:res";
    char const* const kListCmd = "wowparty:cmd";
}

namespace PartyBridge
{
    Bridge& Bridge::instance()
    {
        static Bridge inst;
        return inst;
    }

    Bridge::~Bridge()
    {
        Stop();
    }

    void Bridge::LoadConfig()
    {
        _enabledCfg = sConfigMgr->GetOption<bool>("PartyBridge.Enable", false);
        _host = sConfigMgr->GetOption<std::string>("PartyBridge.RedisHost", "127.0.0.1");
        _port = sConfigMgr->GetOption<int32>("PartyBridge.RedisPort", 6379);
        _snapshotIntervalMs = std::max<uint32>(250, sConfigMgr->GetOption<uint32>("PartyBridge.SnapshotIntervalMs", 1000));
        _maxCommandsPerTick = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PartyBridge.MaxCommandsPerTick", 8), 1, 64);

        _masterAccounts.clear();
        std::string const ids = sConfigMgr->GetOption<std::string>("PartyBridge.MasterAccountIds", "");
        for (std::string_view token : Acore::Tokenize(ids, ',', false))
            if (Optional<uint32> id = Acore::StringTo<uint32>(token))
                _masterAccounts.insert(*id);

#ifndef PARTY_BRIDGE_AVAILABLE
        if (_enabledCfg)
            LOG_WARN("module", "[mod-party-bridge] enabled in config but built WITHOUT hiredis - bridge disabled.");
        _enabledCfg = false;
#endif
    }

    bool Bridge::IsMasterAllowed(uint32 accountId) const
    {
        return _masterAccounts.empty() || _masterAccounts.count(accountId) > 0;
    }

    void Bridge::Start()
    {
        if (!_enabledCfg)
        {
            _active.store(false, std::memory_order_relaxed);
            return;
        }

        if (_running.load(std::memory_order_acquire))
            return;

#ifdef PARTY_BRIDGE_AVAILABLE
        _active.store(true, std::memory_order_relaxed);
        _running.store(true, std::memory_order_release);
        try
        {
            _publisher = std::thread(&Bridge::PublisherMain, this);
            _puller = std::thread(&Bridge::PullerMain, this);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module", "[mod-party-bridge] failed to start worker threads: {}", e.what());
            Stop();
            return;
        }
        LOG_INFO("module", "[mod-party-bridge] active -> redis {}:{} (snapshot every {} ms, {} commands/tick max).",
                 _host, _port, _snapshotIntervalMs, _maxCommandsPerTick);
#endif
    }

    void Bridge::Stop()
    {
        bool const wasRunning = _running.exchange(false, std::memory_order_acq_rel);
        _active.store(false, std::memory_order_relaxed);
        if (!wasRunning)
            return;

        _outCv.notify_all();
        for (std::thread* t : { &_publisher, &_puller })
        {
            if (!t->joinable())
                continue;
            try
            {
                t->join();
            }
            catch (...)
            {
                try { t->detach(); } catch (...) {}   // shutdown must never hang or throw
            }
        }
    }

    void Bridge::Publish(Channel channel, uint32 accountId, std::string payload)
    {
        if (!_active.load(std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(_outMutex);
            if (_out.size() >= kMaxOutQueue)
            {
                _out.pop_front();
                if ((++_dropped % 1000) == 1)
                    LOG_WARN("module", "[mod-party-bridge] outgoing queue full, dropping oldest (total dropped {}).", _dropped);
            }
            _out.push_back(Out{ channel, accountId, std::move(payload) });
        }
        _outCv.notify_one();
    }

    void Bridge::DrainCommands(std::vector<std::string>& out, size_t max)
    {
        std::lock_guard<std::mutex> lk(_inMutex);
        while (!_in.empty() && out.size() < max)
        {
            out.push_back(std::move(_in.front()));
            _in.pop_front();
        }
    }

#ifdef PARTY_BRIDGE_AVAILABLE
    namespace
    {
        // Connects with a timeout; returns nullptr on failure. `cmdTimeoutSec` must exceed the
        // longest blocking command on that context (BRPOP 1 s -> 3 s).
        redisContext* Connect(std::string const& host, int port, int cmdTimeoutSec)
        {
            timeval tv{ 2, 0 };
            redisContext* c = redisConnectWithTimeout(host.c_str(), port, tv);
            if (!c || c->err)
            {
                if (c)
                    redisFree(c);
                return nullptr;
            }
            timeval cmdTv{ cmdTimeoutSec, 0 };
            redisSetTimeout(c, cmdTv);
            return c;
        }
    }

    void Bridge::PublisherMain()
    {
        redisContext* ctx = nullptr;
        uint32 backoffMs = kBackoffMinMs;
        auto nextConnectAt = std::chrono::steady_clock::now();

        auto drop = [&]()
        {
            if (ctx)
                redisFree(ctx);
            ctx = nullptr;
            nextConnectAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoffMs);
            backoffMs = std::min<uint32>(backoffMs * 2, kBackoffMaxMs);
        };

        auto ensureConnected = [&]() -> bool
        {
            if (ctx)
                return true;
            if (std::chrono::steady_clock::now() < nextConnectAt)
                return false;
            ctx = Connect(_host, _port, 2);
            if (!ctx)
            {
                nextConnectAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoffMs);
                backoffMs = std::min<uint32>(backoffMs * 2, kBackoffMaxMs);
                return false;
            }
            backoffMs = kBackoffMinMs;
            LOG_INFO("module", "[mod-party-bridge] publisher connected to redis {}:{}.", _host, _port);
            return true;
        };

        auto run = [&](redisReply* reply) -> bool
        {
            bool const ok = reply && !ctx->err;
            if (reply)
                freeReplyObject(reply);
            return ok;
        };

        while (_running.load(std::memory_order_acquire))
        {
            std::deque<Out> batch;
            {
                std::unique_lock<std::mutex> lk(_outMutex);
                _outCv.wait_for(lk, std::chrono::milliseconds(500), [&]
                {
                    return !_running.load(std::memory_order_acquire) || !_out.empty();
                });
                batch.swap(_out);
            }

            if (batch.empty())
                continue;

            if (!ensureConnected())
                continue;   // Redis down -> drop the batch

            for (Out const& o : batch)
            {
                bool ok = true;
                switch (o.channel)
                {
                    case Channel::Snapshot:
                    {
                        ok = run(static_cast<redisReply*>(
                            redisCommand(ctx, "PUBLISH %s %b", kChanSnap, o.payload.data(), o.payload.size())));
                        if (ok)
                            ok = run(static_cast<redisReply*>(
                                redisCommand(ctx, "SET wowparty:last:%u %b EX 10", o.accountId,
                                             o.payload.data(), o.payload.size())));
                        break;
                    }
                    case Channel::Event:
                        ok = run(static_cast<redisReply*>(
                            redisCommand(ctx, "PUBLISH %s %b", kChanEvt, o.payload.data(), o.payload.size())));
                        break;
                    case Channel::Result:
                        ok = run(static_cast<redisReply*>(
                            redisCommand(ctx, "PUBLISH %s %b", kChanRes, o.payload.data(), o.payload.size())));
                        break;
                }

                if (!ok)
                {
                    drop();   // rest of this batch is dropped by design
                    break;
                }
            }
        }

        if (ctx)
            redisFree(ctx);
    }

    void Bridge::PullerMain()
    {
        redisContext* ctx = nullptr;
        uint32 backoffMs = kBackoffMinMs;

        while (_running.load(std::memory_order_acquire))
        {
            if (!ctx)
            {
                ctx = Connect(_host, _port, 3);
                if (!ctx)
                {
                    // Sleep in small steps so Stop() stays responsive.
                    for (uint32 waited = 0; waited < backoffMs && _running.load(std::memory_order_acquire); waited += 100)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    backoffMs = std::min<uint32>(backoffMs * 2, kBackoffMaxMs);
                    continue;
                }
                backoffMs = kBackoffMinMs;
                LOG_INFO("module", "[mod-party-bridge] command puller connected to redis {}:{}.", _host, _port);
            }

            redisReply* reply = static_cast<redisReply*>(redisCommand(ctx, "BRPOP %s 1", kListCmd));
            if (!reply)
            {
                // Timeout on a healthy link surfaces as REDIS_ERR_IO/EAGAIN too; reconnect either way.
                redisFree(ctx);
                ctx = nullptr;
                continue;
            }

            // BRPOP -> [key, value]; nil (REDIS_REPLY_NIL) on timeout.
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 && reply->element[1]->type == REDIS_REPLY_STRING)
            {
                std::string cmd(reply->element[1]->str, reply->element[1]->len);
                if (cmd.size() <= kMaxCommandBytes)
                {
                    std::lock_guard<std::mutex> lk(_inMutex);
                    if (_in.size() < kMaxInQueue)
                        _in.push_back(std::move(cmd));
                }
                else
                {
                    LOG_WARN("module", "[mod-party-bridge] dropped oversized command ({} bytes).", cmd.size());
                }
            }
            else if (reply->type == REDIS_REPLY_ERROR)
            {
                freeReplyObject(reply);
                redisFree(ctx);
                ctx = nullptr;
                continue;
            }
            freeReplyObject(reply);
        }

        if (ctx)
            redisFree(ctx);
    }
#else
    void Bridge::PublisherMain() {}
    void Bridge::PullerMain() {}
#endif
}
