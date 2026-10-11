// Items on the ground: what a player drops is seen, and can be picked up, by everybody.
//
// Owner  : the player who dropped it. Drops are detected without relying on engine call paths:
//          every 250 ms our characters' inventories are compared with their previous content, and
//          ground items that appear next to one of them shortly after the same kind of item left
//          its inventory are ours. They are announced (MSG_GROUND_ITEM) and watched: when one of
//          our characters picks it up again, or it disappears, MSG_GROUND_REMOVE follows.
// Others : the item is recreated in the dropper's ghost inventory and dropped by it, so it lands
//          where the owner's character stands, through the engine's own drop. When one of our
//          characters picks that copy up, the owner is asked (MSG_GROUND_TAKE); he removes the
//          real item (taker must stand next to it) and tells everybody to remove their copy.
// Two players grabbing the same item within the same round trip each keep one (rare).
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/util/hand.h>
#include <core/Functions.h>

#include "Shared.h"
#include "CharSync.h"
#include "../core/ItemDiff.h"

#include <map>
#include <set>
#include <vector>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD SCAN_PERIOD_MS = 250;
    const float DROP_RADIUS = 12.f;          // a drop lands this close to the character
    const DWORD DROP_WINDOW_MS = 3000;       // inventory loss -> ground item matching window
    const float TAKE_RANGE = 45.f;           // owner's check on the taker (its ghost lags a little)
    const float GHOST_DROP_RANGE = 40.f;     // the dropper's ghost must be this close to drop the copy
    const DWORD REMOTE_TTL_MS = 60000;       // announced items not placeable here are forgotten

    std::string sidOf(GameData* d) { return d ? d->stringID : std::string(); }
    GameData* dataOf(const std::string& sid) { return sid.empty() ? NULL : ou->gamedata.getData(sid); }

    InvItem describe(Item* it)
    {
        InvItem r;
        r.item = sidOf(it->data); r.manufacturer = sidOf(it->manufacturerData); r.material = sidOf(it->materialData);
        r.quantity = it->quantity > 0 ? it->quantity : 1;
        r.quality = it->quality;
        return r;
    }

    // ------------------------------------------------------------------ SEH-guarded engine calls
    bool safeNear(const Ogre::Vector3* p, float r, lektor<RootObject*>* out)
    {
        __try { ou->getObjectsWithinSphere(*out, *p, r, ITEM, 64, NULL); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeLists(Inventory* inv, const lektor<Item*>** loose, lektor<Item*>* weapons, lektor<Item*>* armour)
    {
        __try
        {
            *loose = &inv->getAllItems();
            inv->getEquippedWeapons(*weapons);
            inv->getEquippedArmour(*armour);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Item* safeCreate(GameData* gd, GameData* man, GameData* mat, const hand* h)
    {
        __try { return ou->theFactory->createItem(gd, *h, man, mat, -1, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeDropFrom(Character* c, Item* it, int qty)
    {
        __try
        {
            Inventory* inv = c->inventory;
            if (!inv || !inv->addItem(it, qty, false, true)) return false;
            inv->dropItem(it);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDestroy(Item* it)
    {
        __try { return ou->destroy(it, false, "kenshimp"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeOnGround(Item* it, Ogre::Vector3* pos)
    {
        __try
        {
            if (it->isInInventory) return false;
            *pos = it->getPosition();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // A bag (backpack, sack...) carries its own inventory, travelling with it to the ground.
    Inventory* safeBagInventory(Item* it)
    {
        // By its data type (backpacks, sacks...: CONTAINER); the class type does not say it.
        __try { return it->data && it->data->type == CONTAINER ? static_cast<ContainerItem*>(it)->inventory : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    Item* safeWornBag(Character* c)
    {
        __try { return c->hasABackpackOn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeAddTo(Inventory* inv, Item* it, int qty)
    {
        __try { return inv->addItem(it, qty, false, true); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::vector<InvItem> bagContents(Item* bag)
    {
        std::vector<InvItem> out;
        Inventory* inv = safeBagInventory(bag);
        const lektor<Item*>* loose = NULL;
        lektor<Item*> weapons, armour;
        if (!inv || !safeLists(inv, &loose, &weapons, &armour)) return out;
        for (uint32_t i = 0; i < loose->size() && out.size() < MAX_INV_ITEMS; ++i)
        {
            Item* it = (*loose)[i];
            if (!it) continue;
            InvItem d = describe(it);
            if (d.sanitize()) out.push_back(d);
        }
        return out;
    }

    Item* create(const InvItem& d)
    {
        GameData* gd = dataOf(d.item);
        if (!gd) return NULL;
        GameData* man = dataOf(d.manufacturer);
        GameData* mat = dataOf(d.material);
        hand none;
        Item* it = NULL;
        if (gd->type == WEAPON && man) it = safeCreate(man, gd, mat, &none);   // (manufacturer, weapon)
        if (!it) it = safeCreate(gd, man, mat, &none);
        if (it) { it->quantity = d.quantity; it->quality = d.quality; }
        return it;
    }

    // ------------------------------------------------------------------ owner side
    struct Loss { std::string key; int qty; DWORD at; Ogre::Vector3 where; };
    std::map<uint32_t, std::map<std::string, int> > g_lastContent;   // our characters' content
    std::vector<Loss> g_losses;
    std::vector<Loss> g_gains;                       // same, for what our characters received
    struct Mine { hand h; Item* ptr; InvItem desc; Ogre::Vector3 pos; std::vector<InvItem> contents; };
    std::map<uint32_t, Mine> g_mine;                // our announced ground items
    std::map<uint32_t, InvItem> g_gone;             // ours that left the ground lately (a late pickup is undone)
    void rememberGone(uint32_t id, const InvItem& d)
    {
        if (g_gone.size() > 256) g_gone.clear();
        g_gone[id] = d;
    }
    uint32_t g_nextMine = 1;
    struct Seen { hand h; DWORD firstSeen; };
    std::map<Item*, Seen> g_seen;                    // ground items near us, not (yet) ours

    std::map<std::string, int> contentOf(Character* c)
    {
        std::map<std::string, int> out;
        if (!c->inventory) return out;
        const lektor<Item*>* loose = NULL;
        lektor<Item*> weapons, armour;
        if (!safeLists(c->inventory, &loose, &weapons, &armour)) return out;
        std::set<Item*> seen;
        const lektor<Item*>* lists[3] = { loose, &weapons, &armour };
        if (Item* bag = safeWornBag(c))   // the worn bag is not in those lists
        {
            InvItem d = describe(bag);
            if (seen.insert(bag).second && d.sanitize()) out[itemKindKey(d)] += d.quantity;
        }
        for (int l = 0; l < 3; ++l)
            for (uint32_t i = 0; i < lists[l]->size(); ++i)
            {
                Item* it = (*lists[l])[i];
                if (!it || !seen.insert(it).second) continue;
                InvItem d = describe(it);
                if (d.sanitize()) out[itemKindKey(d)] += d.quantity;
            }
        return out;
    }

    void announce(uint32_t id, const Mine& m)
    {
        ByteWriter w; w.u32(id); m.desc.write(w); w.f32(m.pos.x); w.f32(m.pos.y); w.f32(m.pos.z);
        if (!m.contents.empty())   // optional trailer: what the bag holds
        {
            w.u8(1); w.u16((uint16_t)m.contents.size());
            for (size_t i = 0; i < m.contents.size(); ++i) m.contents[i].write(w);
        }
        g_session.send(MSG_GROUND_ITEM, w.data);
    }
    void announceRemoval(uint32_t id)
    {
        ByteWriter w; w.u32(id);
        g_session.send(MSG_GROUND_REMOVE, w.data);
    }

    bool isRemoteCopy(Item* it);
    bool safeSetPos(Item* it, const Ogre::Vector3* p);

    void ownerScan(DWORD now)
    {
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);

        // 1) What left our characters' inventories (drops, but also eating, crafting, trading...).
        std::set<uint32_t> present;
        for (size_t i = 0; i < locals.size(); ++i)
        {
            uint32_t id = locals[i].first; Character* c = locals[i].second;
            present.insert(id);
            std::map<std::string, int> now_ = contentOf(c);
            std::map<uint32_t, std::map<std::string, int> >::iterator prev = g_lastContent.find(id);
            if (prev != g_lastContent.end())
                for (std::map<std::string, int>::iterator k = prev->second.begin(); k != prev->second.end(); ++k)
                {
                    std::map<std::string, int>::iterator n = now_.find(k->first);
                    int gone = k->second - (n == now_.end() ? 0 : n->second);
                    if (gone > 0) { Loss l; l.key = k->first; l.qty = gone; l.at = now; l.where = c->getPosition(); g_losses.push_back(l); }
                }
            if (prev != g_lastContent.end())
                for (std::map<std::string, int>::iterator n = now_.begin(); n != now_.end(); ++n)
                {
                    std::map<std::string, int>::iterator k = prev->second.find(n->first);
                    int got = n->second - (k == prev->second.end() ? 0 : k->second);
                    if (got > 0) { Loss l; l.key = n->first; l.qty = got; l.at = now; l.where = c->getPosition(); g_gains.push_back(l); }
                }
            g_lastContent[id] = now_;
        }
        for (std::map<uint32_t, std::map<std::string, int> >::iterator it = g_lastContent.begin(); it != g_lastContent.end();)
            if (!present.count(it->first)) g_lastContent.erase(it++); else ++it;
        for (size_t i = g_losses.size(); i-- > 0;)
            if (now - g_losses[i].at > DROP_WINDOW_MS) g_losses.erase(g_losses.begin() + i);
        for (size_t i = g_gains.size(); i-- > 0;)
            if (now - g_gains[i].at > DROP_WINDOW_MS) g_gains.erase(g_gains.begin() + i);

        // 2) Ground items next to our characters: new ones matching a recent loss are our drops.
        std::set<Item*> nearNow;
        for (size_t i = 0; i < locals.size(); ++i)
        {
            Ogre::Vector3 p = locals[i].second->getPosition();
            lektor<RootObject*> found;
            if (!safeNear(&p, DROP_RADIUS, &found)) continue;
            for (uint32_t k = 0; k < found.size(); ++k)
            {
                hand h(found[k]);
                Item* it = h.getItem();
                if (!it) continue;
                nearNow.insert(it);
                if (isRemoteCopy(it)) continue;
                bool known = false;
                for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end() && !known; ++m) known = m->second.ptr == it;
                if (known) continue;
                std::map<Item*, Seen>::iterator s = g_seen.find(it);
                if (s != g_seen.end() && s->second.h.getItem() == it) continue;   // already judged
                Seen sn; sn.h = h; sn.firstSeen = now;
                g_seen[it] = sn;
                InvItem d = describe(it);
                if (!d.sanitize()) continue;
                std::string key = itemKindKey(d);
                for (size_t l = 0; l < g_losses.size(); ++l)
                {
                    if (g_losses[l].key != key || g_losses[l].where.distance(p) > DROP_RADIUS * 2) continue;
                    Mine m; m.h = h; m.ptr = it; m.desc = d; safeOnGround(it, &m.pos); m.contents = bagContents(it);
                    uint32_t id = makeNetId(g_session.localId(), g_nextMine++);
                    g_mine[id] = m;
                    announce(id, m);
                    log("ground: dropped %d x %s announced as %08x (%d item(s) inside)", d.quantity, d.item.c_str(), id, (int)m.contents.size());
                    g_losses[l].qty -= d.quantity;
                    if (g_losses[l].qty <= 0) g_losses.erase(g_losses.begin() + l);
                    g_seen.erase(it);
                    break;
                }
            }
        }
        // Items that were seen but never matched are world items: forget them once out of range.
        for (std::map<Item*, Seen>::iterator s = g_seen.begin(); s != g_seen.end();)
            if (!nearNow.count(s->first) && now - s->second.firstSeen > DROP_WINDOW_MS) g_seen.erase(s++); else ++s;

        // 3) Our announced items: still on the ground?
        for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end();)
        {
            Item* it = m->second.h.getItem();
            Ogre::Vector3 pos;
            if (it != m->second.ptr || !safeOnGround(it, &pos))
            {
                rememberGone(m->first, m->second.desc);
                announceRemoval(m->first);   // picked up by us, or gone
                g_mine.erase(m++);
                continue;
            }
            ++m;
        }
    }

    // ------------------------------------------------------------------ copies of the others' drops
    struct Remote { InvItem desc; Ogre::Vector3 pos; hand h; Item* ptr; bool placed; DWORD since, placedAt; Ogre::Vector3 dropPos; std::vector<InvItem> contents; };
    const DWORD ADOPT_MS = 2000;   // the engine may replace the dropped object: adopt the ground one
    std::map<uint32_t, Remote> g_remote;

    bool isRemoteCopy(Item* it)
    {
        for (std::map<uint32_t, Remote>::iterator r = g_remote.begin(); r != g_remote.end(); ++r)
            if (r->second.placed && r->second.ptr == it) return true;
        return false;
    }

    void placeRemote(uint32_t id, Remote& r)
    {
        Character* ghost = chars_ghostNear(netIdOwner(id), r.pos, GHOST_DROP_RANGE);
        bool exact = false;
        if (!ghost && netIdOwner(id) == HOST_ID) { ghost = chars_anyNear(r.pos, 120.f); exact = ghost != NULL; }   // a world item of the host's ground
        if (!ghost) return;
        Item* it = create(r.desc);
        if (!it) { r.placed = true; r.ptr = NULL; log("ground: unknown item '%s' (missing mod?)", r.desc.item.c_str()); return; }
        ++g_itemsMute; ++g_itemsEpoch;
        if (!r.contents.empty())   // fill the bag before it leaves the ghost's hands
        {
            Inventory* bag = safeBagInventory(it);
            int failed = bag ? 0 : (int)r.contents.size();
            for (size_t i = 0; bag && i < r.contents.size(); ++i)
            {
                Item* c = create(r.contents[i]);
                if (!c || !safeAddTo(bag, c, r.contents[i].quantity)) ++failed;
            }
            if (failed) log("ground: %d of %d item(s) could not be put in the copy of %08x", failed, (int)r.contents.size(), id);
        }
        bool ok = safeDropFrom(ghost, it, r.desc.quantity);
        --g_itemsMute;
        if (!ok) { log("ground: could not drop %s for %08x", r.desc.item.c_str(), id); r.placed = true; r.ptr = NULL; return; }
        r.placed = true; r.ptr = it; r.h = hand(it);
        r.placedAt = GetTickCount(); r.dropPos = ghost->getPosition();
        if ((exact || netIdOwner(id) == HOST_ID) && safeSetPos(it, &r.pos)) r.dropPos = r.pos;   // where it lies in the host's world
        Ogre::Vector3 at;
        bool onGround = safeOnGround(it, &at);
        log("ground: %s dropped by player %d placed (%08x) on ground %d at (%.1f,%.1f,%.1f), ghost at (%.1f,%.1f,%.1f)",
            r.desc.item.c_str(), (int)netIdOwner(id), id, (int)onGround, at.x, at.y, at.z, r.dropPos.x, r.dropPos.y, r.dropPos.z);
    }

    // Right after our drop, the ground object may not be the Item we created: find it by kind.
    bool adopt(uint32_t id, Remote& r)
    {
        lektor<RootObject*> found;
        if (!safeNear(&r.dropPos, 6.f, &found)) return false;
        std::string key = itemKindKey(r.desc);
        for (uint32_t k = 0; k < found.size(); ++k)
        {
            hand h(found[k]);
            Item* it = h.getItem();
            Ogre::Vector3 pos;
            if (!it || isRemoteCopy(it) || !safeOnGround(it, &pos)) continue;
            bool mine = false;
            for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end() && !mine; ++m) mine = m->second.ptr == it;
            if (mine) continue;
            InvItem d = describe(it);
            if (!d.sanitize() || itemKindKey(d) != key) continue;
            r.ptr = it; r.h = h;
            g_seen.erase(it);
            log("ground: copy of %08x adopted after the drop", id);
            return true;
        }
        return false;
    }

    void remoteScan(DWORD now)
    {
        for (std::map<uint32_t, Remote>::iterator it = g_remote.begin(); it != g_remote.end();)
        {
            Remote& r = it->second;
            if (!r.placed)
            {
                placeRemote(it->first, r);
                if (!r.placed && now - r.since > REMOTE_TTL_MS) { g_remote.erase(it++); continue; }
                ++it; continue;
            }
            if (!r.ptr) { ++it; continue; }
            Item* item = r.h.getItem();
            Ogre::Vector3 pos;
            if (item == r.ptr && safeOnGround(item, &pos)) { ++it; continue; }
            if (now - r.placedAt < ADOPT_MS) { adopt(it->first, r); ++it; continue; }   // still settling
            // Left the ground (it may even have merged into a stack and vanished). Picked up by one
            // of our characters if one next to it just received that kind of item.
            std::string key = itemKindKey(r.desc);
            bool ours = false;
            for (size_t g = 0; g < g_gains.size() && !ours; ++g)
            {
                if (g_gains[g].key != key || g_gains[g].where.distance(r.pos) > DROP_RADIUS * 2) continue;
                ours = true;
                g_gains[g].qty -= r.desc.quantity;
                if (g_gains[g].qty <= 0) g_gains.erase(g_gains.begin() + g);
            }
            if (ours)
            {
                ByteWriter w; w.u8(netIdOwner(it->first)); w.u32(it->first);
                g_session.send(MSG_GROUND_TAKE, w.data);
                log("ground: picked up %s (%08x), owner told", r.desc.item.c_str(), it->first);
                g_remote.erase(it++);
                continue;
            }
            if (item != r.ptr) { r.ptr = NULL; ++it; continue; }   // unloaded here: nothing to report
            g_remote.erase(it++);   // in some other inventory (NPC...): forget it
        }
    }
    // ------------------------------------------------------------------ KenshiKin: the host's ground
    // Kenshi rolls loot at random in each world, so what lies on the ground (things NPCs dropped,
    // items left in ruins...) differed from one player to the next; only players' own drops were
    // shared. Now the host's ground is the shared one:
    //  Host   : every second, the items lying around each client in the shared world are announced
    //           to that client (MSG_GROUND_ITEM, host-owned ids), and tracked like the host's own
    //           drops: picked up / gone -> MSG_GROUND_REMOVE to everybody, a client's pickup goes
    //           through MSG_GROUND_TAKE as for any drop.
    //  Client : such a copy is dropped by the nearest character and put at the host's exact spot
    //           (Item::setPositionRotation). The client's own world items around its characters
    //           (never its own drops, never copies) are removed once they are clearly not ours.
    // Bags lying on the ground (dropped by a player or the world) also share their content while
    // they lie there (CONTAINER_GROUND_BAG, Items.cpp): their owner sends it on change.
    const DWORD WORLD_SCAN_MS = 1000, WORLD_PURGE_MS = 1000, BAG_PUSH_MS = 500;
    const float WORLD_RADIUS = 80.f;          // host: around each of a client's characters
    const float WORLD_MERGE = 50.f;
    const float PURGE_RADIUS = 60.f;          // client: around our characters
    const DWORD PURGE_AGE_MS = 10000;         // an item must be seen this long before it is judged
    const DWORD OWN_LOSS_MEMORY_MS = 60000;   // ... and no item of its kind left one of ours near it
    const float OWN_BASE_RADIUS = 150.f;      // ... and not near one of our own buildings
    const size_t MAX_WORLD_ITEMS = 1500;
    DWORD g_lastWorldScan = 0, g_lastPurge = 0, g_lastBagPush = 0;
    long g_worldAnnounced = 0, g_worldPurged = 0;
    std::map<uint32_t, std::set<uint8_t> > g_sentTo;   // host: world items -> clients that have them
    std::map<uint32_t, uint32_t> g_bagHash;            // our bags on the ground: content last sent
    std::vector<Loss> g_recentLosses;                  // client: what left our characters lately (60 s)
    std::map<Item*, Seen> g_purgeSeen;                 // client: world items around us, since when

    bool safeSetPos(Item* it, const Ogre::Vector3* p)
    {
        __try { it->setPositionRotation(*p, Ogre::Quaternion::IDENTITY, false); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void sendItemTo(uint8_t client, uint32_t id, const Mine& m)
    {
        ByteWriter w; w.u32(id); m.desc.write(w); w.f32(m.pos.x); w.f32(m.pos.y); w.f32(m.pos.z);
        if (!m.contents.empty())
        {
            w.u8(1); w.u16((uint16_t)m.contents.size());
            for (size_t i = 0; i < m.contents.size(); ++i) m.contents[i].write(w);
        }
        g_session.sendTo(client, MSG_GROUND_ITEM, w.data);
    }

    void hostWorldScan(DWORD now)
    {
        std::set<Item*> mine;
        for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end(); ++m) mine.insert(m->second.ptr);
        size_t worldCount = g_sentTo.size();
        std::vector<PlayerInfo> players = g_session.players();
        for (size_t p = 0; p < players.size(); ++p)
        {
            uint8_t client = players[p].id;
            if (client == HOST_ID || npcs_clientOwnWorld(client)) continue;
            std::vector<Ogre::Vector3> chars, centres;
            cs_ghostPositions(client, chars);
            for (size_t i = 0; i < chars.size(); ++i)
            {
                bool covered = false;
                for (size_t k = 0; k < centres.size() && !covered; ++k) covered = centres[k].distance(chars[i]) < WORLD_MERGE;
                if (!covered) centres.push_back(chars[i]);
            }
            int fresh = 0;
            for (size_t c = 0; c < centres.size(); ++c)
            {
                lektor<RootObject*> found;
                if (!safeNear(&centres[c], WORLD_RADIUS, &found)) continue;
                for (uint32_t k = 0; k < found.size(); ++k)
                {
                    hand h(found[k]);
                    Item* it = h.getItem();
                    Ogre::Vector3 pos;
                    if (!it || isRemoteCopy(it) || !safeOnGround(it, &pos)) continue;
                    std::map<Item*, Seen>::iterator s = g_seen.find(it);
                    if (s != g_seen.end() && now - s->second.firstSeen < DROP_WINDOW_MS) continue;   // maybe our own fresh drop
                    uint32_t id = 0;
                    if (mine.count(it))
                    {
                        for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end(); ++m)
                            if (m->second.ptr == it) { id = m->first; break; }
                        if (!g_sentTo.count(id)) continue;   // our own drop: already announced to everybody
                    }
                    else
                    {
                        if (worldCount >= MAX_WORLD_ITEMS || fresh >= 64) continue;
                        InvItem d = describe(it);
                        if (!d.sanitize()) continue;
                        Mine m; m.h = h; m.ptr = it; m.desc = d; m.pos = pos; m.contents = bagContents(it);
                        id = makeNetId(HOST_ID, g_nextMine++ & 0x7FFFFF);
                        g_mine[id] = m;
                        g_sentTo[id];
                        mine.insert(it);
                        g_seen.erase(it);
                        ++worldCount; ++fresh; ++g_worldAnnounced;
                    }
                    std::set<uint8_t>& sent = g_sentTo[id];
                    if (sent.insert(client).second) sendItemTo(client, id, g_mine[id]);
                }
            }
        }
        static DWORD lastLog = 0;
        static long lastCount = 0;
        if (g_worldAnnounced != lastCount && now - lastLog > 30000)
        { lastLog = now; log("ground: %ld world item(s) of the host shared so far (%u tracked)", g_worldAnnounced, (unsigned)g_sentTo.size()); lastCount = g_worldAnnounced; }
    }

    // Client: the nearest character drops the copy, then it is put at the host's spot.
    Character* anyCharacterNear(const Ogre::Vector3& pos, float range)
    {
        return chars_anyNear(pos, range);
    }

    // Client: our world's own items around us (not ours, not copies) go.
    void clientPurge(DWORD now)
    {
        for (size_t i = g_recentLosses.size(); i-- > 0;)
            if (now - g_recentLosses[i].at > OWN_LOSS_MEMORY_MS) g_recentLosses.erase(g_recentLosses.begin() + i);
        for (size_t i = 0; i < g_losses.size(); ++i)
        {
            bool known = false;
            for (size_t k = 0; k < g_recentLosses.size() && !known; ++k)
                known = g_recentLosses[k].at == g_losses[i].at && g_recentLosses[k].key == g_losses[i].key;
            if (!known) g_recentLosses.push_back(g_losses[i]);
        }
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);
        std::set<Item*> mine;
        for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end(); ++m) mine.insert(m->second.ptr);
        std::vector<Ogre::Vector3> base;   // our own buildings: what we keep on the floor there is ours
        builds_localPositions(base);
        int removed = 0;
        std::set<Item*> seenNow;
        for (size_t i = 0; i < locals.size(); ++i)
        {
            if (!locals[i].second) continue;
            Ogre::Vector3 p = locals[i].second->getPosition();
            lektor<RootObject*> found;
            if (!safeNear(&p, PURGE_RADIUS, &found)) continue;
            for (uint32_t k = 0; k < found.size(); ++k)
            {
                hand h(found[k]);
                Item* it = h.getItem();
                Ogre::Vector3 pos;
                if (!it || mine.count(it) || isRemoteCopy(it) || !safeOnGround(it, &pos)) continue;
                seenNow.insert(it);
                std::map<Item*, Seen>::iterator s = g_purgeSeen.find(it);
                if (s == g_purgeSeen.end() || s->second.h.getItem() != it) { Seen sn; sn.h = h; sn.firstSeen = now; g_purgeSeen[it] = sn; continue; }
                if (now - s->second.firstSeen < PURGE_AGE_MS) continue;
                InvItem d = describe(it);
                if (!d.sanitize()) continue;
                std::string key = itemKindKey(d);
                bool ownLoss = false;
                for (size_t l = 0; l < g_recentLosses.size() && !ownLoss; ++l)
                    ownLoss = g_recentLosses[l].key == key && g_recentLosses[l].where.distance(pos) < DROP_RADIUS * 3;
                if (ownLoss) continue;   // could be ours after all: never destroy what we may have dropped
                bool atBase = false;
                for (size_t b = 0; b < base.size() && !atBase; ++b) atBase = base[b].distance(pos) < OWN_BASE_RADIUS;
                if (atBase) continue;
                ++g_itemsMute; ++g_itemsEpoch;
                bool ok = safeDestroy(it);
                --g_itemsMute;
                if (ok) { ++removed; ++g_worldPurged; g_purgeSeen.erase(it); seenNow.erase(it); }
            }
        }
        for (std::map<Item*, Seen>::iterator s = g_purgeSeen.begin(); s != g_purgeSeen.end();)
            if (!seenNow.count(s->first)) g_purgeSeen.erase(s++); else ++s;
        static DWORD lastLog = 0;
        if (removed && now - lastLog > 30000) { lastLog = now; log("ground: %ld item(s) of our own world removed (the host's ground is shown)", g_worldPurged); }
    }

    // Our bags on the ground (drops and, on the host, world items): their content to the others.
    void pushBags(DWORD now)
    {
        for (std::map<uint32_t, Mine>::iterator m = g_mine.begin(); m != g_mine.end(); ++m)
        {
            Inventory* bag = safeBagInventory(m->second.ptr);
            if (!bag || m->second.h.getItem() != m->second.ptr) continue;
            Bytes body = items_inventoryMsg(CONTAINER_GROUND_BAG, m->first, bag);
            uint32_t h = hashBytes(body);
            std::map<uint32_t, uint32_t>::iterator old = g_bagHash.find(m->first);
            if (old != g_bagHash.end() && old->second == h) continue;
            g_bagHash[m->first] = h;
            std::map<uint32_t, std::set<uint8_t> >::iterator to = g_sentTo.find(m->first);
            if (to == g_sentTo.end()) g_session.send(MSG_INVENTORY, body);   // our drop: everybody has it
            else for (std::set<uint8_t>::iterator c = to->second.begin(); c != to->second.end(); ++c) g_session.sendTo(*c, MSG_INVENTORY, body);
        }
        for (std::map<uint32_t, uint32_t>::iterator it = g_bagHash.begin(); it != g_bagHash.end();)
            if (!g_mine.count(it->first)) g_bagHash.erase(it++); else ++it;
    }
}

namespace
{
    bool safeGiveFromGround(Character* c, Item* it)
    {
        __try { return c->giveItem(it, false, false); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeWalkTo(Character* c, const Ogre::Vector3* p)
    {
        __try { c->getMovement()->_setPositionAndTeleport(*p, 0); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

namespace
{
    hand g_testBag;
    bool safeEquipBag(Character* c, Item* it)
    {
        __try { return c->inventory && c->inventory->equipItem(it); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDropOnly(Character* c, Item* it)
    {
        __try { if (!c->inventory) return false; c->inventory->dropItem(it); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

// Test runs: a backpack with two items inside, given to c (dropped a few seconds later, so the
// owner scan sees it arrive, then leave).
std::string ground_debugGiveBag(Character* c)
{
    lektor<GameData*> bags, things;
    ou->gamedata.getDataOfType(bags, CONTAINER);
    ou->gamedata.getDataOfType(things, ITEM);
    GameData* bagData = NULL;
    for (uint32_t i = 0; i < bags.size() && !bagData; ++i)
        if (bags[i] && bags[i]->name.find("Backpack") != std::string::npos) bagData = bags[i];
    if (!bagData || things.size() == 0 || !things[0]) return "no backpack / item data";
    InvItem b; b.item = bagData->stringID; b.quantity = 1;
    Item* bag = create(b);
    if (!bag) return "could not create " + b.item;
    // A bag gets its own inventory once worn: put it on, then fill it.
    if (!c->inventory || !safeAddTo(c->inventory, bag, 1)) return "could not give the bag";
    bool worn = safeEquipBag(c, bag);
    Item* wornBag = safeWornBag(c);
    if (wornBag) bag = wornBag;
    Inventory* inside = safeBagInventory(bag);
    int put = 0;
    for (int k = 0; k < 2 && inside; ++k)
    {
        InvItem t; t.item = things[k < (int)things.size() ? k : 0]->stringID; t.quantity = 1;
        Item* it = create(t);
        if (it && safeAddTo(inside, it, 1)) ++put;
    }
    g_testBag = hand(bag);
    char buf[200]; sprintf_s(buf, "%s worn=%d inventory=%d with %d item(s) inside", bagData->name.c_str(), (int)worn, (int)(inside != NULL), put);
    return buf;
}

std::string ground_debugDropBag(Character* c)
{
    Item* bag = g_testBag.getItem();
    if (!bag) return "no test bag";
    return safeDropOnly(c, bag) ? "dropped" : "could not drop";
}

std::string ground_debugPickup(Character* taker)
{
    Ogre::Vector3 p = taker->getPosition();
    lektor<RootObject*> found;
    if (!safeNear(&p, 40.f, &found)) return "query failed";
    log("ground pickup: %d object(s) of type ITEM within 40 of (%.1f,%.1f,%.1f), %d remote copies known", (int)found.size(), p.x, p.y, p.z, (int)g_remote.size());
    for (std::map<uint32_t, Remote>::iterator it = g_remote.begin(); it != g_remote.end(); ++it)
    {
        Item* ri = it->second.h.getItem(); Ogre::Vector3 rp;
        bool og = ri && safeOnGround(ri, &rp);
        log("ground pickup:   copy %08x valid %d on ground %d at (%.1f,%.1f,%.1f)", it->first, (int)(ri != NULL), (int)og, rp.x, rp.y, rp.z);
    }
    Item* best = NULL; float bestD = 1e9f;
    for (uint32_t k = 0; k < found.size(); ++k)
    {
        hand h(found[k]);
        Item* it = h.getItem();
        Ogre::Vector3 ip;
        if (!it || !safeOnGround(it, &ip)) continue;
        float d = ip.distance(p);
        if (d < bestD) { bestD = d; best = it; }
    }
    // Other players' drops further away (debug: as if our character walked there first).
    Ogre::Vector3 bestPos = p;
    for (std::map<uint32_t, Remote>::iterator it = g_remote.begin(); it != g_remote.end(); ++it)
    {
        Item* ri = it->second.h.getItem(); Ogre::Vector3 rp;
        if (!ri || ri != it->second.ptr || !safeOnGround(ri, &rp)) continue;
        float d = rp.distance(p);
        if (d < 150.f && (!best || d < bestD)) { bestD = d; best = ri; bestPos = rp; }
    }
    if (!best) return "no item on the ground nearby";
    if (bestD > 3.f)
    {
        Ogre::Vector3 next = bestPos + Ogre::Vector3(1.5f, 0, 0);
        safeWalkTo(taker, &next);
    }
    std::string name = describe(best).item;
    return safeGiveFromGround(taker, best) ? "picked up " + name : "could not pick up " + name;
}

void ground_tick(DWORD now)
{
    static DWORD last = 0;
    if (now - last < SCAN_PERIOD_MS || !ready() || !g_session.active()) return;
    last = now;
    ownerScan(now);
    remoteScan(now);
    if (g_cfg.worldGround && g_cfg.townSync && ready())
    {
        if (g_session.isHost() && now - g_lastWorldScan >= WORLD_SCAN_MS) { g_lastWorldScan = now; hostWorldScan(now); }
        if (!g_session.isHost() && !npcs_ownWorld() && now - g_lastPurge >= WORLD_PURGE_MS) { g_lastPurge = now; clientPurge(now); }
    }
    if (ready() && now - g_lastBagPush >= BAG_PUSH_MS) { g_lastBagPush = now; pushBags(now); }
    for (std::map<uint32_t, std::set<uint8_t> >::iterator it = g_sentTo.begin(); it != g_sentTo.end();)
        if (!g_mine.count(it->first)) g_sentTo.erase(it++); else ++it;
}

Inventory* ground_bagInventory(uint32_t id)
{
    std::map<uint32_t, Mine>::iterator m = g_mine.find(id);
    if (m != g_mine.end()) return m->second.h.getItem() == m->second.ptr ? safeBagInventory(m->second.ptr) : NULL;
    std::map<uint32_t, Remote>::iterator r = g_remote.find(id);
    if (r != g_remote.end() && r->second.placed && r->second.ptr && r->second.h.getItem() == r->second.ptr) return safeBagInventory(r->second.ptr);
    return NULL;
}

bool ground_itemPosition(uint32_t id, Ogre::Vector3& out)
{
    std::map<uint32_t, Mine>::iterator m = g_mine.find(id);
    if (m != g_mine.end()) return m->second.h.getItem() == m->second.ptr && safeOnGround(m->second.ptr, &out);
    std::map<uint32_t, Remote>::iterator r = g_remote.find(id);
    if (r != g_remote.end()) { out = r->second.pos; return true; }
    return false;
}

void ground_onMessage(const NetEvent& e)
{
    ByteReader r(e.body);
    if (e.msgType == MSG_GROUND_ITEM)
    {
        uint32_t id = r.u32();
        Remote rm; rm.desc.read(r);
        float x = r.f32(), y = r.f32(), z = r.f32();
        if (!r.ok() || netIdOwner(id) != e.sender || !rm.desc.sanitize() || !validPos(x, y, z)) return;
        if (r.remaining() && r.u8() == 1)
        {
            uint16_t n = r.u16();
            if (!r.ok() || n > MAX_INV_ITEMS) return;
            for (uint16_t i = 0; i < n && r.ok(); ++i) { InvItem d; d.read(r); if (d.sanitize()) rm.contents.push_back(d); }
            if (!r.ok()) return;
        }
        if (g_remote.count(id)) return;
        rm.pos = Ogre::Vector3(x, y, z); rm.ptr = NULL; rm.placed = false; rm.since = GetTickCount();
        g_remote[id] = rm;
        placeRemote(id, g_remote[id]);
        return;
    }
    if (e.msgType == MSG_GROUND_REMOVE)
    {
        uint32_t id = r.u32();
        if (!r.ok() || netIdOwner(id) != e.sender) return;
        std::map<uint32_t, Remote>::iterator it = g_remote.find(id);
        if (it == g_remote.end()) return;
        Item* item = it->second.h.getItem();
        Ogre::Vector3 pos;
        if (item && item == it->second.ptr && safeOnGround(item, &pos)) safeDestroy(item);
        g_remote.erase(it);
        return;
    }
    if (e.msgType == MSG_GROUND_TAKE)
    {
        uint8_t target = r.u8(); uint32_t id = r.u32();
        if (!r.ok() || target != g_session.localId()) return;
        std::map<uint32_t, Mine>::iterator it = g_mine.find(id);
        if (it == g_mine.end())
        {
            // Already gone (someone else picked it up first): the taker must give its copy up.
            std::map<uint32_t, InvItem>::iterator g = g_gone.find(id);
            if (g != g_gone.end()) items_sendUndo(e.sender, UNDO_REMOVE, CONTAINER_GROUND, id, g->second);
            return;
        }
        Item* item = it->second.h.getItem();
        Ogre::Vector3 pos;
        if (item != it->second.ptr || !safeOnGround(item, &pos))
        {
            items_sendUndo(e.sender, UNDO_REMOVE, CONTAINER_GROUND, id, it->second.desc);
            return;
        }
        std::vector<Ogre::Vector3> ghosts;
        cs_ghostPositions(e.sender, ghosts);
        bool near_ = false;
        for (size_t i = 0; i < ghosts.size() && !near_; ++i) near_ = ghosts[i].distance(pos) <= TAKE_RANGE;
        if (!near_)
        {
            log("ground: refused pickup of %08x by %s (not next to it)", id, playerName(e.sender).c_str());
            items_sendUndo(e.sender, UNDO_REMOVE, CONTAINER_GROUND, id, it->second.desc);
            announce(id, it->second);   // the taker's copy is gone: it gets it back on the ground
            return;
        }
        log("ground: %s picked up %s (%08x)", playerName(e.sender).c_str(), it->second.desc.item.c_str(), id);
        safeDestroy(item);
        announceRemoval(id);
        rememberGone(id, it->second.desc);
        g_mine.erase(it);
    }
}

void ground_resendAll()
{
    for (std::map<uint32_t, Mine>::iterator it = g_mine.begin(); it != g_mine.end(); ++it) announce(it->first, it->second);
}

void ground_onPlayerLeft(uint8_t id)
{
    // Host: what it had of our ground goes with it (it gets it again when it comes back near).
    for (std::map<uint32_t, std::set<uint8_t> >::iterator it = g_sentTo.begin(); it != g_sentTo.end(); ++it) it->second.erase(id);
    // Its drops stay in its own world; our copies go.
    for (std::map<uint32_t, Remote>::iterator it = g_remote.begin(); it != g_remote.end();)
    {
        if (netIdOwner(it->first) != id) { ++it; continue; }
        Item* item = it->second.h.getItem();
        Ogre::Vector3 pos;
        if (item && item == it->second.ptr && safeOnGround(item, &pos)) safeDestroy(item);
        g_remote.erase(it++);
    }
}

void ground_onWorldReload()
{
    g_remote.clear();
    g_mine.clear();
    g_gone.clear();
    g_seen.clear();
    g_losses.clear();
    g_gains.clear();
    g_lastContent.clear();
    g_sentTo.clear();
    g_bagHash.clear();
    g_recentLosses.clear();
    g_purgeSeen.clear();
}

} // namespace kmp
