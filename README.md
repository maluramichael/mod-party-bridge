# mod-party-bridge

An [AzerothCore](https://www.azerothcore.org/) module for the
[Playerbots](https://github.com/liyunfan1223/mod-playerbots) fork (WotLK 3.3.5a): a live
**party dashboard bridge** over Redis.

## What it does

- Publishes a JSON snapshot of the bots in each real player's group to Redis (channel
  `wowparty:snap`, roughly once per second).
- Executes whitelisted commands that a web dashboard queues on the Redis list
  `wowparty:cmd`. Commands are injected as a whisper from the master to the bot, so the
  Playerbots engine's own validation and security stay in charge.

Redis I/O runs entirely on background threads — the world thread never touches a socket. If
Redis is down the module simply drops data (no crash, no lag).

## Requirements

- The Playerbots fork of AzerothCore.
- A reachable Redis instance (built with `hiredis`).

## Configuration

`conf/mod_party_bridge.conf.dist`: `PartyBridge.Enable`, `RedisHost`, `RedisPort`,
`SnapshotIntervalMs`, `MaxCommandsPerTick`, `MasterAccountIds`.

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver.

## License

Released under the GNU GPL v2 (or later).
