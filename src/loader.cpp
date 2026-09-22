// mod-party-bridge module loader.
// The core's generated script loader calls Addmod_party_bridgeScripts();
// that in turn registers this module's scripts.

void AddPartyBridgeScripts();
void AddPartyEventScripts();

void Addmod_party_bridgeScripts()
{
    AddPartyBridgeScripts();
    AddPartyEventScripts();
}
