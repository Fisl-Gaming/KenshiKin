// KenshiKin: town doors and gates, open / closed / locked, as in the host's world.
//
// Upstream KenshiMP only shared broken doors and destroyed buildings (Buildings.cpp). Whether a
// door or a town gate is open or locked stayed whatever each client's own save said, and since a
// client's own town residents are removed (Npcs.cpp), nobody there ever opened or locked anything:
// a gate open for the host could stay shut for a client, and the other way round.
//
//  Host   : every SCAN_MS, the world buildings with doors around each client in the shared world
//           are read (locked / open per door); what changed for that client is sent to it
//           (MSG_DOOR_STATES), everything again every REFRESH_MS.
//  Client : applies the host's states to its copies around its characters. It never closes a door
//           one of its own characters is walking through (the host sees our body pass and opens
//           its own copy soon after). A door that got unlocked here while the host says locked,
//           with one of our characters next to it, was lockpicked: reported to the host
//           (MSG_DOOR_REPORT), which unlocks its copy if that player really stands there.
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/GameData.h>
#include <kenshi/Character.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Building/DoorStuff.h>
#include <core/Functions.h>

#include "Shared.h"
#include "CharSync.h"

#include <map>
#include <vector>
#include <math.h>
#include <stdio.h>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD SCAN_MS = 1500;            // host: doors around each client read this often
    const DWORD REFRESH_MS = 20000;        // host: unchanged states resent this often
    const DWORD APPLY_MS = 500;            // client: host's states applied this often
    const float SCAN_RADIUS = 250.f;       // host: around each character of a client
    const float CENTRE_MERGE = 120.f;      // host: characters this close share one query
    const float APPLY_RANGE = 350.f;       // client: only doors around our characters
    const float BODY_IN_DOORWAY = 7.f;     // client: never close a door one of ours is this close to
    const float PICK_RANGE = 10.f;         // client: a door unlocked with one of ours this close was picked
    const float REPORT_RANGE = 20.f;       // host: the reporter's character must be this close
    const DWORD REPORT_COOLDOWN_MS = 3000;
    const uint16_t MAX_PER_MSG = 300;
    const size_t MAX_TABLE = 4096;

    enum { BIT_LOCKED = 1, BIT_OPEN = 2, BIT_BROKEN = 4 };

    struct DoorSet
    {
        std::string sid;
        float x, y, z;
        std::vector<uint8_t> bits;        // per door, the building's door order
        DoorSet() : x(0), y(0), z(0) {}
        void write(ByteWriter& w) const
        {
            w.str(sid); w.f32(x); w.f32(y); w.f32(z);
            w.u8((uint8_t)bits.size()); for (size_t k = 0; k < bits.size(); ++k) w.u8(bits[k]);
        }
        void read(ByteReader& r)
        {
            sid = r.str(); x = r.f32(); y = r.f32(); z = r.f32();
            uint8_t n = r.u8(); bits.clear(); for (int k = 0; k < n && r.ok(); ++k) bits.push_back(r.u8() & 7);
        }
        bool sane() const { return !sid.empty() && validPos(x, y, z) && !bits.empty(); }
    };

    std::string keyOf(const std::string& sid, float x, float y, float z)
    {
        char buf[96];
        sprintf_s(buf, "@%d,%d,%d", (int)floorf(x + 0.5f), (int)floorf(y + 0.5f), (int)floorf(z + 0.5f));
        return sid + buf;
    }

    // ---- engine access (SEH: no C++ objects with destructors in these frames) -----------------
    int g_engineFailures = 0;             // open/close calls that threw: feature off after a few
    bool engineOk() { return g_engineFailures < 5; }

    DoorStuff* doorAt(Building* b, uint32_t i)
    {
        if (!b || i >= b->doors.size()) return NULL;
        Building* d = b->doors[i];
        return d && d->isDoor() ? static_cast<DoorStuff*>(d) : NULL;
    }
    bool safeIsTownBuilding(Building* b)
    {
        __try { return b->data && !b->isFurnitureOrDoor() && b->doors.size() > 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDoorBits(DoorStuff* d, uint8_t* bits)
    {
        __try
        {
            uint8_t v = 0;
            if (d->isBroken()) v |= BIT_BROKEN;
            if (d->isLocked()) v |= BIT_LOCKED;
            if (d->state == DOORSTATE_OPEN || d->state == DOORSTATE_OPENING) v |= BIT_OPEN;
            *bits = v;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDoorPos(DoorStuff* d, Ogre::Vector3* p)
    {
        __try { *p = d->getPosition(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeLock(DoorStuff* d, bool on)
    {
        __try { if (on) d->lockDoor(); else d->unlockDoor(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++g_engineFailures; return false; }
    }
    bool safeOpen(DoorStuff* d, bool on)
    {
        __try { if (on) d->openDoor(); else d->closeDoor(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++g_engineFailures; return false; }
    }

    bool captureDoors(Building* b, DoorSet& out)
    {
        if (!b || !safeIsTownBuilding(b) || builds_netIdOf(b)) return false;   // players' buildings: Buildings.cpp
        Ogre::Vector3 p = b->getPosition();
        out.sid = b->data->stringID; out.x = p.x; out.y = p.y; out.z = p.z;
        out.bits.clear();
        for (uint32_t i = 0; i < b->doors.size() && i < 64; ++i)
        {
            uint8_t v = 0;
            DoorStuff* d = doorAt(b, i);
            if (d && !safeDoorBits(d, &v)) return false;
            out.bits.push_back(v);
        }
        return !out.sid.empty();
    }

    // ---- host ------------------------------------------------------------------------------
    std::map<uint8_t, std::map<std::string, std::vector<uint8_t> > > g_sent;   // per client: what it has
    std::map<uint8_t, DWORD> g_lastRefresh;
    DWORD g_lastScan = 0;

    void sendSets(uint8_t client, const std::vector<DoorSet>& sets)
    {
        for (size_t start = 0; start < sets.size(); start += MAX_PER_MSG)
        {
            size_t end = start + MAX_PER_MSG < sets.size() ? start + MAX_PER_MSG : sets.size();
            ByteWriter w; w.u16((uint16_t)(end - start));
            for (size_t k = start; k < end; ++k) sets[k].write(w);
            g_session.sendTo(client, MSG_DOOR_STATES, w.data);
        }
    }

    void hostScan(DWORD now)
    {
        std::vector<PlayerInfo> players = g_session.players();
        for (size_t p = 0; p < players.size(); ++p)
        {
            uint8_t client = players[p].id;
            if (client == HOST_ID || npcs_clientOwnWorld(client)) continue;
            std::vector<Ogre::Vector3> chars, centres;
            cs_ghostPositions(client, chars);
            for (size_t i = 0; i < chars.size(); ++i)
            {
                bool near_ = false;
                for (size_t k = 0; k < centres.size() && !near_; ++k) near_ = centres[k].distance(chars[i]) < CENTRE_MERGE;
                if (!near_) centres.push_back(chars[i]);
            }
            if (centres.empty()) continue;

            std::map<std::string, std::vector<uint8_t> >& sent = g_sent[client];
            if (now - g_lastRefresh[client] >= REFRESH_MS) { g_lastRefresh[client] = now; sent.clear(); }
            std::map<std::string, bool> seen;
            std::vector<DoorSet> changed;
            for (size_t c = 0; c < centres.size(); ++c)
            {
                lektor<RootObject*> list;
                ou->getObjectsWithinSphere(list, centres[c], SCAN_RADIUS, BUILDING, 400, NULL);
                for (uint32_t i = 0; i < list.size(); ++i)
                {
                    DoorSet s;
                    if (!captureDoors(static_cast<Building*>(list[i]), s)) continue;
                    std::string key = keyOf(s.sid, s.x, s.y, s.z);
                    if (seen.count(key)) continue;
                    seen[key] = true;
                    std::map<std::string, std::vector<uint8_t> >::iterator it = sent.find(key);
                    if (it != sent.end() && it->second == s.bits) continue;
                    if (sent.size() >= MAX_TABLE) sent.clear();
                    sent[key] = s.bits;
                    changed.push_back(s);
                }
            }
            if (!changed.empty())
            {
                sendSets(client, changed);
                static DWORD lastLog = 0;
                if (g_cfg.debugKeys || now - lastLog > 60000) { lastLog = now; log("doors: %u town building(s) sent to %s", (unsigned)changed.size(), playerName(client).c_str()); }
            }
        }
    }

    // A client lockpicked (or locked) a town door of the host's world.
    void hostOnReport(uint8_t sender, ByteReader& r)
    {
        DoorSet s; s.read(r);
        uint8_t door = r.u8();
        if (!r.ok() || !s.sane() || door >= s.bits.size() || npcs_clientOwnWorld(sender)) return;
        Building* b = builds_findWorld(s.sid, s.x, s.y, s.z);
        DoorStuff* d = b && safeIsTownBuilding(b) && !builds_netIdOf(b) ? doorAt(b, door) : NULL;
        Ogre::Vector3 dp;
        if (!d || !safeDoorPos(d, &dp)) return;
        std::vector<Ogre::Vector3> theirs;
        cs_ghostPositions(sender, theirs);
        bool there = false;
        for (size_t i = 0; i < theirs.size() && !there; ++i) there = theirs[i].distance(dp) <= REPORT_RANGE;
        if (!there) { log("doors: report from %s refused (none of its characters at that door)", playerName(sender).c_str()); return; }
        bool wantLocked = (s.bits[door] & BIT_LOCKED) != 0;
        uint8_t cur = 0;
        if (!safeDoorBits(d, &cur) || ((cur & BIT_LOCKED) != 0) == wantLocked) return;
        if (safeLock(d, wantLocked))
        {
            log("doors: %s %s a door of %s", playerName(sender).c_str(), wantLocked ? "locked" : "unlocked", s.sid.c_str());
            for (std::map<uint8_t, std::map<std::string, std::vector<uint8_t> > >::iterator it = g_sent.begin(); it != g_sent.end(); ++it)
                it->second.erase(keyOf(s.sid, s.x, s.y, s.z));   // everybody gets the new state at the next scan
        }
    }

    // ---- client ----------------------------------------------------------------------------
    struct Entry
    {
        DoorSet st;
        hand h;
        std::vector<DWORD> reportedAt;   // per door
    };
    std::map<std::string, Entry> g_table;
    DWORD g_lastApply = 0;
    long g_applied = 0;

    bool sharedClient() { return !g_session.isHost() && !npcs_ownWorld(); }

    void report(const Entry& e, uint32_t door, uint8_t bitsHere)
    {
        DoorSet s = e.st;
        s.bits[door] = bitsHere;
        ByteWriter w; w.u8(HOST_ID); s.write(w); w.u8((uint8_t)door);
        g_session.sendTo(HOST_ID, MSG_DOOR_REPORT, w.data);
    }

    void clientApply(DWORD now)
    {
        std::vector<std::pair<uint32_t, Character*> > mine;
        chars_localCharacters(mine);
        std::vector<Ogre::Vector3> ours;
        for (size_t i = 0; i < mine.size(); ++i) if (mine[i].second) ours.push_back(mine[i].second->getPosition());
        if (ours.empty()) return;
        int lookups = 0;
        for (std::map<std::string, Entry>::iterator it = g_table.begin(); it != g_table.end(); ++it)
        {
            Entry& e = it->second;
            Ogre::Vector3 bp(e.st.x, e.st.y, e.st.z);
            bool inRange = false;
            for (size_t i = 0; i < ours.size() && !inRange; ++i) inRange = ours[i].distance(bp) < APPLY_RANGE;
            if (!inRange) continue;
            Building* b = e.h.getBuilding();
            if (!b)
            {
                if (++lookups > 30) break;   // the rest next time
                b = builds_findWorld(e.st.sid, e.st.x, e.st.y, e.st.z);
                if (!b) continue;
                e.h = hand(b);
            }
            if (e.reportedAt.size() != e.st.bits.size()) e.reportedAt.assign(e.st.bits.size(), 0);
            for (uint32_t i = 0; i < b->doors.size() && i < e.st.bits.size(); ++i)
            {
                DoorStuff* d = doorAt(b, i);
                uint8_t cur = 0, want = e.st.bits[i];
                Ogre::Vector3 dp;
                if (!d || !safeDoorBits(d, &cur) || !safeDoorPos(d, &dp)) continue;
                if ((cur | want) & BIT_BROKEN) continue;            // broken doors: Buildings.cpp
                float nearest = 1e30f;
                for (size_t k = 0; k < ours.size(); ++k) { float dd = ours[k].distance(dp); if (dd < nearest) nearest = dd; }
                bool wantLocked = (want & BIT_LOCKED) != 0, isLocked = (cur & BIT_LOCKED) != 0;
                if (wantLocked && !isLocked && nearest < PICK_RANGE)
                {
                    // Unlocked here, next to one of ours: lockpicked. The host decides.
                    if (now - e.reportedAt[i] > REPORT_COOLDOWN_MS) { e.reportedAt[i] = now; report(e, i, cur); log("doors: lockpicked door of %s reported to the host", e.st.sid.c_str()); }
                    continue;
                }
                if (wantLocked != isLocked)
                {
                    if (wantLocked && (cur & BIT_OPEN) && nearest >= BODY_IN_DOORWAY) safeOpen(d, false);
                    if (safeLock(d, wantLocked)) ++g_applied;
                    continue;
                }
                bool wantOpen = (want & BIT_OPEN) != 0, isOpen = (cur & BIT_OPEN) != 0;
                if (wantOpen == isOpen || isLocked) continue;
                if (!wantOpen && nearest < BODY_IN_DOORWAY) continue;   // one of ours is walking through it
                if (safeOpen(d, wantOpen)) ++g_applied;
            }
        }
    }
}

void doors_tick(DWORD now)
{
    if (!g_cfg.doorSync || !g_cfg.townSync || !ready() || !engineOk()) return;
    if (g_session.isHost())
    {
        if (now - g_lastScan >= SCAN_MS) { g_lastScan = now; hostScan(now); }
        return;
    }
    if (!sharedClient()) return;
    if (now - g_lastApply >= APPLY_MS) { g_lastApply = now; clientApply(now); }
    static DWORD lastLog = 0;
    if (g_applied && now - lastLog > 60000) { lastLog = now; log("doors: %ld door change(s) applied from the host's world", g_applied); g_applied = 0; }
}

void doors_onMessage(const NetEvent& e)
{
    if (!g_cfg.doorSync || !g_cfg.townSync) return;
    ByteReader r(e.body);
    if (e.msgType == MSG_DOOR_STATES)
    {
        if (g_session.isHost() || e.sender != HOST_ID) return;
        uint16_t n = r.u16();
        if (!r.ok() || n > MAX_PER_MSG) return;
        for (uint16_t i = 0; i < n && r.ok(); ++i)
        {
            DoorSet s; s.read(r);
            if (!r.ok() || !s.sane()) continue;
            std::string key = keyOf(s.sid, s.x, s.y, s.z);
            if (!g_table.count(key) && g_table.size() >= MAX_TABLE) continue;
            Entry& en = g_table[key];
            en.st = s;
        }
        return;
    }
    if (e.msgType == MSG_DOOR_REPORT && g_session.isHost())
    {
        if (r.u8() != HOST_ID || !r.ok()) return;
        hostOnReport(e.sender, r);
    }
}

void doors_onPlayerLeft(uint8_t id)
{
    g_sent.erase(id);
    g_lastRefresh.erase(id);
}

// World reloaded / session over: every cached pointer is stale, clients get everything again.
void doors_reset()
{
    g_sent.clear();
    g_lastRefresh.clear();
    g_table.clear();
}

void doors_resendAll() { g_sent.clear(); }

} // namespace kmp
