#include "game/stamina.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

// What a blocked hit costs, for the player only.
//
// The game picks a blocked hit's cost from six engine settings by how hard
// the hit was, and the same code runs for whoever is blocking. Through 1.0.11
// GuardPercent wrote those six, so at the default of 0 every enemy's block
// was free as well, and a shield never broke however long it was hit (#16).
// The six are left at the game's values now. A hook at the end of the code
// that picks one scales the cost in eax when the blocker is the character
// being played, and every other blocker pays what the game charges.
//
// The hook runs on a game thread for every blocked hit anywhere in the world.
// It never writes the log: it counts, stores what it saw in a ring, and a
// thread of its own writes those out ten times a second.

using namespace sm::sig;

namespace sm::stamina
{
    namespace
    {
        enum Who : uint8_t
        {
            kPlayerSame,    // the blocker is the played actor
            kPlayerParts,   // a different pointer with the played actor's component array
            kOther,         // somebody else
            kNoPlayer,      // the played actor could not be read
            kNoBlocker,     // the blocker could not be read
        };

        const char* WhoText(uint8_t w)
        {
            switch (w)
            {
            case kPlayerSame:  return "the player (same actor), scaled";
            case kPlayerParts: return "the player (same component array, different actor pointer), scaled";
            case kOther:       return "another character, the game's cost";
            case kNoPlayer:    return "no played character could be read, the game's cost";
            default:           return "the blocker could not be read, the game's cost";
            }
        }

        struct Ev
        {
            DWORD tick;
            DWORD thread;
            uintptr_t ref, ent, entComps, me, meComps;
            uint32_t entEid, meEid;
            int cost, out;
            uint8_t tier, who;
        };

        constexpr unsigned kRing = 8192;
        Ev       g_ring[kRing];
        unsigned g_ringN = 0;          // events waiting, under g_ringLock
        long     g_ringDropped = 0;    // events the ring had no room for
        SRWLOCK  g_ringLock = SRWLOCK_INIT;

        int  g_percent = 100;
        bool g_probe = false;
        bool g_hooked = false;
        uintptr_t g_hookAt = 0;

        uintptr_t g_ctlGlobal = 0;
        unsigned  g_ctlController = 0, g_ctlActor = 0;

        std::atomic<long> g_hits{0}, g_player{0}, g_other{0}, g_unknown{0};

        HANDLE g_logThread = nullptr;
        HANDLE g_logStop = nullptr;

        uintptr_t Controlled()
        {
            if (!g_ctlGlobal) return 0;
            const uintptr_t svcs = mem::Deref(g_ctlGlobal, 0);
            const uintptr_t svc  = svcs ? mem::Deref(svcs, kOff_ControlSite_Svc) : 0;
            const uintptr_t ctl  = svc ? mem::Deref(svc, g_ctlController) : 0;
            return ctl ? mem::Deref(ctl, g_ctlActor) : 0;
        }

        // On the game thread, at the cdqe after the cost is picked.
        void OnBlockedHit(farhook::MidRegs* r)
        {
            mem::FaultSite site("guard hook");
            Ev e{};
            e.tick = GetTickCount();
            e.thread = GetCurrentThreadId();
            e.cost = static_cast<int>(static_cast<uint32_t>(r->rax));
            e.out = e.cost;
            e.tier = static_cast<uint8_t>(r->rcx);
            e.ref = r->r15;
            if (!mem::ReadPtr(e.ref, &e.ent)) e.ent = 0;
            if (e.ent)
            {
                e.entComps = mem::Deref(e.ent, kOff_Ent_Comps);
                mem::Read32(e.ent + kOff_Ent_Eid, &e.entEid);
            }
            e.me = Controlled();
            if (e.me)
            {
                e.meComps = mem::Deref(e.me, kOff_Ent_Comps);
                mem::Read32(e.me + kOff_Ent_Eid, &e.meEid);
            }

            if (!e.ent) e.who = kNoBlocker;
            else if (!e.me) e.who = kNoPlayer;
            else if (e.ent == e.me) e.who = kPlayerSame;
            else if (e.entComps && e.entComps == e.meComps) e.who = kPlayerParts;
            else e.who = kOther;

            if (e.who == kPlayerSame || e.who == kPlayerParts)
            {
                e.out = static_cast<int>(static_cast<int64_t>(e.cost) * g_percent / 100);
                r->rax = (r->rax & 0xFFFFFFFF00000000ull) | static_cast<uint32_t>(e.out);
                ++g_player;
            }
            else if (e.who == kOther) ++g_other;
            else ++g_unknown;
            ++g_hits;

            AcquireSRWLockExclusive(&g_ringLock);
            if (g_ringN < kRing) g_ring[g_ringN++] = e;
            else ++g_ringDropped;
            ReleaseSRWLockExclusive(&g_ringLock);
        }

        bool g_toldPlayer = false, g_toldOther = false, g_toldUnknown = false;

        void Drain()
        {
            static Ev batch[kRing];
            AcquireSRWLockExclusive(&g_ringLock);
            const unsigned n = g_ringN;
            if (n) memcpy(batch, g_ring, n * sizeof(Ev));
            g_ringN = 0;
            ReleaseSRWLockExclusive(&g_ringLock);

            for (unsigned i = 0; i < n; ++i)
            {
                const Ev& e = batch[i];
                const bool player = e.who == kPlayerSame || e.who == kPlayerParts;
                if (player && !g_toldPlayer)
                {
                    g_toldPlayer = true;
                    LOG_OK("[guard] first blocked hit by the player this session: strength %u, cost %d became "
                           "%d at GuardPercent=%d.", e.tier, e.cost, e.out, g_percent);
                }
                else if (e.who == kOther && !g_toldOther)
                {
                    g_toldOther = true;
                    LOG_OK("[guard] first blocked hit by another character this session: strength %u, cost %d "
                           "kept at the game's value.", e.tier, e.cost);
                }
                else if ((e.who == kNoPlayer || e.who == kNoBlocker) && !g_toldUnknown)
                {
                    g_toldUnknown = true;
                    LOG("[guard] first blocked hit whose blocker could not be matched: %s.", WhoText(e.who));
                }
                if (g_probe)
                    LOG("[guard] blocked hit, strength %u, thread %lu: blocker ref 0x%p actor 0x%p id 0x%08X "
                        "parts 0x%p%s | played actor 0x%p id 0x%08X parts 0x%p%s | cost %d became %d, %s",
                        e.tier, e.thread, reinterpret_cast<void*>(e.ref), reinterpret_cast<void*>(e.ent),
                        e.entEid, reinterpret_cast<void*>(e.entComps), e.ent && e.entComps ? "" : " (zero)",
                        reinterpret_cast<void*>(e.me), e.meEid, reinterpret_cast<void*>(e.meComps),
                        e.me && e.meComps ? "" : " (zero)", e.cost, e.out, WhoText(e.who));
            }
        }

        DWORD WINAPI LogThread(LPVOID)
        {
            DWORD lastSummary = GetTickCount();
            long summarised = 0;
            while (WaitForSingleObject(g_logStop, 100) == WAIT_TIMEOUT)
            {
                Drain();
                if (GetTickCount() - lastSummary >= 5 * 60 * 1000)
                {
                    lastSummary = GetTickCount();
                    if (g_hits.load() != summarised)
                    {
                        summarised = g_hits.load();
                        GuardSummary("in the last five minutes and before");
                    }
                }
            }
            Drain();
            return 0;
        }

        // The controller route, read out of the accessor's own instructions.
        bool ResolveController()
        {
            size_t hits = 0;
            const uintptr_t site = mem::FindUnique(kSig_ControlSite, &hits);
            const uintptr_t g = site ? mem::RipAt(site, 7) : 0;
            const uintptr_t acc = site ? mem::RipAt(site + kOff_ControlSite_Call, 5) : 0;
            unsigned ctl = 0, act = 0;
            for (unsigned i = 0; acc && i < kControlAccessorScan && (!ctl || !act); ++i)
            {
                uint8_t d8 = 0; uint32_t d32 = 0;
                if (!ctl && mem::MatchAt(acc + i, "48 8B 79") && mem::Read8(acc + i + 3, &d8)) ctl = d8;
                else if (ctl && !act && mem::MatchAt(acc + i, "4C 8B B0") && mem::Read32(acc + i + 3, &d32) &&
                         d32 < 0x1000)
                    act = d32;
            }
            if (!g || !mem::InImage(g) || !ctl || !act)
            {
                LOG_ERR("[guard] the played character could not be found on this build (%zu control site "
                        "hit%s, controller +0x%X, actor +0x%X), so GuardPercent cannot tell the player's "
                        "blocks from anyone else's. Every blocked hit costs what the game charges.",
                        hits, hits == 1 ? "" : "s", ctl, act);
                return false;
            }
            g_ctlGlobal = g; g_ctlController = ctl; g_ctlActor = act;
            LOG("[guard] played character: global +0x%llX, accessor +0x%llX, controller at +0x%X, its actor "
                "at +0x%X", static_cast<unsigned long long>(mem::Rva(g)),
                static_cast<unsigned long long>(mem::Rva(acc)), ctl, act);
            return true;
        }
    }

    bool ApplyGuard(int percent, bool probe, bool lastTry)
    {
        if (percent == 100)
        {
            LOG("[guard] GuardPercent is 100, so a blocked hit costs what the game charges.");
            return true;
        }

        size_t hits = 0;
        const uintptr_t code = mem::FindUnique(kSig_GuardCostTiers, &hits);
        if (!code)
        {
            if (hits == 0 && !lastTry) return false;
            LOG_ERR("[guard] the code that picks a blocked hit's cost matched %zu times instead of once, "
                    "so this build has changed it. Blocked hits keep the game's cost and everything "
                    "else in this mod still applies.", hits);
            return true;
        }

        // The six as the game has them, for the log. Nothing writes them.
        int now[kGuardCostTiers] = {};
        for (int i = 0; i < kGuardCostTiers; ++i)
        {
            uint32_t disp = 0, value = 0;
            if (mem::Read32(code + kOff_GuardCost_Disp[i], &disp) &&
                mem::Read32(code + kOff_GuardCost_End[i] + static_cast<int32_t>(disp), &value))
                now[i] = static_cast<int>(value);
        }
        LOG("[guard] the six blocked hit costs are left at the game's values, light to heavy with the "
            "fallback last: %d %d %d %d %d %d (0 means the game has not filled them in yet).",
            now[0], now[1], now[2], now[3], now[4], now[5]);

        // r15 has to be the blocker at the hook. On 3840 it is loaded from
        // the first argument's home slot well ahead of the chain.
        bool r15 = false;
        for (unsigned back = 7; back <= kGuardBlockerSearchBack && !r15; ++back)
            r15 = mem::MatchAt(code - back, kSig_GuardBlockerToR15);
        if (!r15)
        {
            LOG_ERR("[guard] the code before the blocked hit cost no longer loads the blocker into r15, so "
                    "this build has changed it. Blocked hits keep the game's cost and everything else in "
                    "this mod still applies.");
            return true;
        }

        if (!ResolveController()) return true;

        g_percent = percent;
        g_probe = probe;
        char why[96];
        const uintptr_t at = code + kOff_GuardCost_Hook;
        if (!farhook::InstallMid("guard cost", at, OnBlockedHit, why, sizeof why))
        {
            LOG_ERR("[guard] the player-only hook at +0x%llX did not go in (%s). Blocked hits keep the "
                    "game's cost.", static_cast<unsigned long long>(mem::Rva(at)), why);
            return true;
        }
        g_hooked = true;
        g_hookAt = at;
        g_logStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_logStop) g_logThread = CreateThread(nullptr, 0, LogThread, nullptr, 0, nullptr);

        LOG_OK("[guard] player-only hook in at +0x%llX. A hit the player blocks costs %d%% of what the game "
               "charges; every other character's blocks cost what the game charges.%s",
               static_cast<unsigned long long>(mem::Rva(at)), percent,
               probe ? " Probe=1, so every blocked hit gets a line." : "");
        return true;
    }

    void GuardSummary(const char* when)
    {
        if (!g_hooked) return;
        AcquireSRWLockShared(&g_ringLock);
        const long dropped = g_ringDropped;
        ReleaseSRWLockShared(&g_ringLock);
        LOG("[guard] %s: %ld blocked hit%s, %ld by the player at %d%%, %ld by other characters at the game's "
            "cost, %ld with a blocker that could not be matched.%s", when, g_hits.load(),
            g_hits.load() == 1 ? "" : "s", g_player.load(), g_percent, g_other.load(), g_unknown.load(),
            dropped ? " Some hits came faster than the log thread and have no line of their own." : "");
        if (dropped) LOG("[guard] %ld blocked hit lines were skipped for lack of room.", dropped);
    }

    void StopGuard()
    {
        if (!g_hooked) return;
        farhook::RemoveAll();
        if (g_logStop) SetEvent(g_logStop);
        if (g_logThread)
        {
            WaitForSingleObject(g_logThread, 2000);
            CloseHandle(g_logThread);
            g_logThread = nullptr;
        }
        GuardSummary("at unload");
        g_hooked = false;
    }
}
