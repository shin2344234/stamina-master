#include "game/stamina.h"

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/mem.h"
#include "game/signatures.h"
#include "game/tables.h"

using namespace sm::sig;
using sm::tables::Table;

namespace
{
    struct Slot
    {
        Table byName, byRtti;
        bool told = false;
        int  nameMisses = 0;     // passes on which no getter named the table
        int  rowWaits = 0;       // passes with the getter's global but no rows yet
        bool toldWait = false, toldFallback = false;
    };
    Slot g_statusSlot, g_skillSlot;

    // Says once per table which route opened it. statusinfo by its resolver
    // means kSig_TableResolver16 still fits this build; by its manager means
    // the pattern missed and the RTTI fallback covered for it, which works but
    // is the first sign of a patch. On 3663 skill opens by its resolver too;
    // before that build only its manager could reach it.
    void Announce(const char* label, Slot& s, const Table& t)
    {
        if (s.told) return;
        s.told = true;
        LOG("[table] %s opened by its %s: global +0x%llX, %u rows, %s at +0x%X", label,
            t.viaRtti ? "manager's RTTI" : "resolver",
            static_cast<unsigned long long>(sm::mem::Rva(t.global)), t.rows,
            t.rowEntries ? "row entries" : "defs", t.defsOff);
    }

    // How many one-second passes to wait on a getter whose table has no rows
    // yet before trying the RTTI route as well. The slowest 3663 load seen took
    // 33 seconds to fill the tables, and the worker gives up at 120.
    constexpr int kRowWaitsBeforeRtti = 60;
    // Passes after which a name no getter answers to stops being searched for.
    // Before 3663 nothing names skill, and the full-image pattern search is
    // not worth repeating every second for the whole wait.
    constexpr int kNameMissesBeforeGivingUp = 3;

    // The getter is the normal route for both tables on 3663. It reads the
    // table's own global, which is a handful of reads, where the RTTI route
    // scans 8.4 MB of .sxdata and asks the page map about every value in it
    // that looks like a pointer. That scan is where 1.0.8's faults came from,
    // so it runs only when no getter names the table, or one has named it and
    // the rows have stayed empty for a minute.
    bool Open(const char* label, const char* name, const char* rtti, Slot& s, Table& out)
    {
        if (name && s.nameMisses < kNameMissesBeforeGivingUp)
        {
            if (sm::tables::Resolve(name, s.byName))
            {
                out = s.byName;
                Announce(label, s, out);
                return true;
            }
            // A miss counts only once statusinfo has been found by name, which
            // shows the code is unpacked and the search works; a miss before
            // that says nothing about whether this table has a getter.
            if (!s.byName.global) { if (&s != &g_statusSlot && g_statusSlot.byName.global) ++s.nameMisses; }
            else if (++s.rowWaits < kRowWaitsBeforeRtti)
            {
                if (!s.toldWait)
                {
                    s.toldWait = true;
                    LOG("[table] %s: its getter names global +0x%llX, and the table is not built yet; "
                        "waiting on it", label, static_cast<unsigned long long>(sm::mem::Rva(s.byName.global)));
                }
                return false;
            }
            else if (!s.toldFallback)
            {
                s.toldFallback = true;
                LOG_ERR("[table] %s: the getter's global +0x%llX has held no readable rows for %d passes, "
                        "so the manager's RTTI is tried as well", label,
                        static_cast<unsigned long long>(sm::mem::Rva(s.byName.global)), s.rowWaits);
            }
        }
        if (rtti && sm::tables::ResolveByManager(rtti, s.byRtti))
        {
            out = s.byRtti;
            Announce(label, s, out);
            return true;
        }
        return false;
    }

    bool OpenBoth(Table& status, Table& skill)
    {
        return Open("statusinfo", kStr_StatusTable, kRtti_StatusManager, g_statusSlot, status) &&
               Open("skill", kStr_SkillTable, kRtti_SkillManager, g_skillSlot, skill);
    }

    // {items, size, capacity}, as the reader at RVA 0x017A1810 writes it on 3663.
    struct List
    {
        uintptr_t items = 0;
        uint32_t  size  = 0;
        uint32_t  cap   = 0;
    };

    bool ReadList(uintptr_t rec, unsigned off, List& out)
    {
        out = List{};
        uint64_t p = 0;
        if (!sm::mem::Read64(rec + off + kOff_List_Items, &p)) return false;
        if (!sm::mem::Read32(rec + off + kOff_List_Size, &out.size)) return false;
        if (!sm::mem::Read32(rec + off + kOff_List_Cap, &out.cap)) return false;
        if (!out.size) return true;                       // empty is a real answer
        if (out.size > 64 || out.cap < out.size) return false;
        if (!sm::mem::Plausible(static_cast<uintptr_t>(p)) ||
            !sm::mem::Readable(static_cast<uintptr_t>(p), 1ull * out.size * kRec_UseResourceStatBytes))
            return false;
        out.items = static_cast<uintptr_t>(p);
        return true;
    }

    uintptr_t EntryAt(const List& l, unsigned i)
    {
        return l.items + 1ull * i * kRec_UseResourceStatBytes;
    }

    // The Stamina row's _statusIndex, which is what an entry stores. Found by
    // the hash the record carries rather than by its name: the hash is what the
    // game matches on, it is four bytes at a known offset, and it was checked
    // offline against hashlittle over the lowercase name.
    bool IndexByHash(const Table& status, uint32_t hash, uint16_t& out)
    {
        for (uint32_t r = 0; r < status.rows; ++r)
        {
            uint32_t h = 0;
            const uintptr_t def = sm::tables::Def(status, r);
            if (!def || !sm::mem::Read32(def + kOff_Status_KeyHash, &h) || h != hash) continue;
            uint32_t idx = 0;
            if (!sm::mem::Read32(def + kOff_Status_Index, &idx) || idx > 0xFFFF) return false;
            out = static_cast<uint16_t>(idx);
            return true;
        }
        return false;
    }

    bool StaminaIndex(const Table& status, uint16_t& out) { return IndexByHash(status, kHash_Stamina, out); }

    // Spirit's index, or 0xFFFF when no row hashes to it. A missing Spirit row
    // costs the Spirit setting and nothing else, so it is not a reason to stop
    // the stamina write.
    uint16_t SpiritIndex(const Table& status)
    {
        uint16_t out = 0;
        return IndexByHash(status, kHash_Spirit, out) ? out : 0xFFFF;
    }

    const unsigned kLists[2] = { kOff_Skill_UseResourceStatList, kOff_Skill_UseDriverResourceStatList };

    // How many entries name Stamina, over the whole table, writing nothing.
    int CountEntries(const Table& skill, uint16_t want)
    {
        int n = 0;
        for (uint32_t r = 0; r < skill.rows; ++r)
        {
            const uintptr_t rec = sm::tables::Def(skill, r);
            if (!rec) continue;
            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l)) continue;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    uint16_t idx = 0;
                    if (sm::mem::Read16(EntryAt(l, i) + kOff_URS_StatusInfo, &idx) && idx == want) ++n;
                }
            }
        }
        return n;
    }

    // The table object and its row count exist before the rows are filled in.
    // On 3424 one session opened skill with all 2158 rows counted, found
    // Skill_Looting and none of the movement rows, walked no stamina entry at
    // all, and gave up for the session; a minute later every cost was there.
    // A walk over a table half filled would have been worse, rewriting the
    // rows that had arrived and leaving the rest at full cost. So the table
    // counts as loaded once the six named rows are in it and the number of
    // Stamina entries is the same on two passes a second apart.
    bool SkillLoaded(const Table& skill, uint16_t want)
    {
        static int s_lastSeen = -1, s_passes = 0;
        ++s_passes;
        for (const char* key : { kSkillKey_Sprint, kSkillKey_Climb, kSkillKey_Swim,
                                 kSkillKey_Glide, kSkillKey_Horse, kSkillKey_Gallop })
            if (sm::tables::RowByKey(skill, key) < 0) { s_lastSeen = -1; return false; }
        const int seen = CountEntries(skill, want);
        const bool steady = seen > 0 && seen == s_lastSeen;
        s_lastSeen = seen;
        if (steady)
            LOG("[stamina] skill holds the six named rows and %d Stamina entries on two passes in a row, "
                "so it counts as loaded (pass %d).", seen, s_passes);
        return steady;
    }
}

namespace sm::stamina
{
    bool SettingsInRange(const Scale& scale)
    {
        for (int v : { scale.oneOff, scale.continuous, scale.mount, scale.spirit, scale.guard })
            if (v < 0 || v > 100) return false;
        return scale.mountRegen >= 100 && scale.mountRegen <= 10000;
    }

    int Apply(const Scale& scale, bool verbose)
    {
        const struct { int v; const char* name; } settings[] = {
            { scale.oneOff,     "UsePercent" },
            { scale.continuous, "ContinuousPercent" },
            { scale.mount,      "MountPercent" },
            { scale.spirit,     "SpiritPercent" },
            { scale.guard,      "GuardPercent" },
        };
        for (const auto& s : settings)
            if (s.v < 0 || s.v > 100)
            {
                LOG_ERR("[stamina] %s is %d, which is outside 0 to 100. Nothing is written, including the "
                        "settings that were in range: a cost scaled past its own size is not something "
                        "this mod knows the game survives.", s.name, s.v);
                return -1;
            }
        if (scale.mountRegen < 100 || scale.mountRegen > 10000)
        {
            LOG_ERR("[stamina] MountRegenPercent is %d, which is outside 100 to 10000. Nothing is "
                    "written. Below 100 it would make a horse tire faster, which is not what this mod "
                    "is for.", scale.mountRegen);
            return -1;
        }
        if (scale.oneOff == 100 && scale.continuous == 100 && scale.mount == 100 &&
            scale.mountRegen == 100 && scale.spirit == 100)
        {
            LOG("[stamina] every skill table setting is 100, so the table keeps the game's own "
                "numbers and nothing in it is written.");
            return 0;
        }

        Table status, skill;
        if (!OpenBoth(status, skill)) return 0;      // not loaded yet, ask again

        uint16_t want = 0;
        if (!StaminaIndex(status, want))
        {
            LOG_ERR("[stamina] no statusinfo row hashes to 0x%08X, so the Stamina status could not be "
                    "named and nothing is written.", kHash_Stamina);
            return -1;
        }
        if (!SkillLoaded(skill, want)) return 0;

        uint16_t spirit = 0xFFFF;
        if (scale.spirit != 100)
        {
            spirit = SpiritIndex(status);
            if (spirit == 0xFFFF)
                LOG_ERR("[stamina] SpiritPercent is %d, but no statusinfo row hashes to 0x%08X, so Spirit "
                        "could not be named and its costs are left alone. Stamina is unaffected.",
                        scale.spirit, kHash_Spirit);
            else
                LOG("[stamina] Spirit is statusinfo row %u; its costs go to %d%%.", spirit, scale.spirit);
        }

        int changed = 0, skippedPositive = 0, failed = 0;
        int spiritKept = 0;            // Spirit entries that give Spirit back, left alone
        int byKind[5] = {};            // one-off, continuous, mount, mount regen, spirit
        static const char* kKind[5] = { "one-off", "continuous", "mount", "mount regen", "spirit" };
        for (uint32_t r = 0; r < skill.rows; ++r)
        {
            const uintptr_t rec = tables::Def(skill, r);
            if (!rec) continue;
            uint8_t applyType = 0;
            mem::Read8(rec + kOff_Skill_ApplyType, &applyType);

            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l) || !l.size) continue;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    const uintptr_t e = EntryAt(l, i);
                    uint16_t idx = 0;
                    uint64_t raw = 0;
                    uint8_t regen = 0;
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx)) continue;
                    const bool isSpirit = spirit != 0xFFFF && idx == spirit;
                    if (idx != want && !isSpirit) continue;
                    if (!mem::Read64(e + kOff_URS_VaryStatAmount, &raw)) continue;
                    mem::Read8(e + kOff_URS_IsRegen, &regen);

                    const int64_t before = static_cast<int64_t>(raw);

                    int kind, percent;
                    if (isSpirit)
                    {
                        // One setting for every Spirit cost. Nobody has asked
                        // for Spirit split by kind, and a positive amount is a
                        // give-back that stays as it is.
                        if (before > 0) { ++spiritKept; continue; }
                        if (before == 0) continue;
                        kind = 4;
                        percent = scale.spirit;
                    }
                    // Spending is negative. A positive amount gives stamina
                    // back, and the only one of those this mod touches is a
                    // mounted regen rate, because that is what decides whether
                    // a horse tires. Every other give-back is left alone, so
                    // food and rest restore what they did.
                    else if (before > 0)
                    {
                        if (applyType != 1 || scale.mountRegen == 100) { ++skippedPositive; continue; }
                        kind = 3;
                        percent = scale.mountRegen;
                    }
                    else if (before == 0) continue;
                    else
                    {
                        // Mount wins over continuous: a mounted skill is a
                        // mounted skill whether it is held or tapped, and
                        // someone setting MountPercent means the horse.
                        kind = (applyType == 1) ? 2 : (regen ? 1 : 0);
                        percent = (kind == 2) ? scale.mount
                                : (kind == 1) ? scale.continuous
                                              : scale.oneOff;
                    }
                    if (percent == 100) continue;

                    const int64_t after = before * percent / 100;
                    if (after == before) continue;
                    if (!tables::WriteI64(e + kOff_URS_VaryStatAmount, after)) { ++failed; continue; }

                    uint64_t back = 0;
                    mem::Read64(e + kOff_URS_VaryStatAmount, &back);
                    if (static_cast<int64_t>(back) != after) { ++failed; continue; }
                    ++changed;
                    ++byKind[kind];
                    // Every one of them. This ran under a cap of 400 lines
                    // through 1.0.8, which 2.05.00's 364 came close to.
                    if (verbose)
                    {
                        char key[96] = "(no key)";
                        tables::StringKey(skill, r, key, sizeof key);
                        LOG("[stamina]   %-44s %-10s %lld -> %lld", key, kKind[kind],
                            static_cast<long long>(before), static_cast<long long>(after));
                    }
                }
            }
        }

        if (!changed)
        {
            LOG_ERR("[stamina] no stamina cost was rewritten. Either the skill table holds none this "
                    "session or every write was refused; %d write(s) failed.", failed);
            return -1;
        }
        LOG_OK("[stamina] %d number%s rewritten: %d one-off at %d%%, %d continuous at %d%%, "
               "%d mounted at %d%%, %d mounted regen at %d%%, %d spirit at %d%%.%s", changed,
               changed == 1 ? "" : "s", byKind[0], scale.oneOff, byKind[1], scale.continuous, byKind[2],
               scale.mount, byKind[3], scale.mountRegen, byKind[4], scale.spirit,
               failed ? " Some writes were refused; see above." : "");
        if (spiritKept)
            LOG("[stamina] %d Spirit entr%s carried a positive amount and %s left alone.", spiritKept,
                spiritKept == 1 ? "y" : "ies", spiritKept == 1 ? "was" : "were");
        if (skippedPositive)
            LOG("[stamina] %d entr%s carried a positive amount and %s left alone. Those give stamina "
                "rather than spend it, so food and rest still restore what they did.", skippedPositive,
                skippedPositive == 1 ? "y" : "ies", skippedPositive == 1 ? "was" : "were");
        if (!verbose)
            LOG("[stamina] Set Probe=1 in the ini for a line per skill changed.");
        return changed;
    }

    namespace
    {
        uintptr_t g_guardAt[kGuardCostTiers] = {};
        int g_guardWant[kGuardCostTiers] = {};
        bool g_guardWritten = false;

        bool WriteInt(uintptr_t at, int v)
        {
            __try { *reinterpret_cast<volatile int*>(at) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    bool ApplyGuard(int percent, bool lastTry)
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

        const uintptr_t base = mem::Game().base;
        const uintptr_t end = base + mem::Game().size;
        int before[kGuardCostTiers] = {};
        uintptr_t at[kGuardCostTiers] = {};
        for (int i = 0; i < kGuardCostTiers; ++i)
        {
            uint32_t disp = 0, value = 0;
            if (!mem::Read32(code + kOff_GuardCost_Disp[i], &disp))
            {
                LOG_ERR("[guard] could not read the address of cost %d. Blocked hits keep the game's cost.", i);
                return true;
            }
            at[i] = code + kOff_GuardCost_End[i] + static_cast<int32_t>(disp);
            if (at[i] < base || at[i] + sizeof(int) > end || !mem::Read32(at[i], &value) ||
                static_cast<int>(value) < 0 || static_cast<int>(value) > kGuardCost_MaxPlausible)
            {
                LOG_ERR("[guard] cost %d at 0x%p holds %d, which is not a stamina cost. Nothing is written "
                        "and blocked hits keep the game's cost.", i, reinterpret_cast<void*>(at[i]),
                        static_cast<int>(value));
                return true;
            }
            // The six live in memory the exe leaves zeroed until a static
            // constructor fills in 10 to 300, so 0 means not set up yet. Taken
            // as the game's cost, it would be what RecheckGuard enforces for
            // the rest of the session.
            if (value == 0)
            {
                if (!lastTry) return false;
                LOG_ERR("[guard] cost %d at 0x%p still reads 0 after the wait, so the game never set it "
                        "up or something else zeroed it. Nothing is written and blocked hits keep "
                        "whatever the game charges.", i, reinterpret_cast<void*>(at[i]));
                return true;
            }
            before[i] = static_cast<int>(value);
        }

        int failed = 0;
        for (int i = 0; i < kGuardCostTiers; ++i)
        {
            const int want = static_cast<int>(static_cast<int64_t>(before[i]) * percent / 100);
            uint32_t back = 0;
            if (!WriteInt(at[i], want) || !mem::Read32(at[i], &back) || static_cast<int>(back) != want)
                ++failed;
            g_guardAt[i] = at[i];
            g_guardWant[i] = want;
        }
        g_guardWritten = true;

        LOG_OK("[guard] blocked hit costs at %d%%, light to heavy with the fallback last: "
               "%d %d %d %d %d %d became %d %d %d %d %d %d.%s", percent,
               before[0], before[1], before[2], before[3], before[4], before[5],
               g_guardWant[0], g_guardWant[1], g_guardWant[2], g_guardWant[3], g_guardWant[4],
               g_guardWant[5], failed ? " Some writes did not land." : "");
        return true;
    }

    void RecheckGuard(const char* when)
    {
        if (!g_guardWritten) return;
        int moved = 0;
        for (int i = 0; i < kGuardCostTiers; ++i)
        {
            uint32_t now = 0;
            if (!mem::Read32(g_guardAt[i], &now)) continue;
            if (static_cast<int>(now) == g_guardWant[i]) continue;
            LOG("[guard] %s cost %d held %d instead of %d, so something in the game wrote it. Putting "
                "it back.", when, i, static_cast<int>(now), g_guardWant[i]);
            WriteInt(g_guardAt[i], g_guardWant[i]);
            ++moved;
        }
        if (!moved) LOG("[guard] %s all six blocked hit costs still hold.", when);
    }

    void Verify(const char* when)
    {
        Table status, skill;
        if (!OpenBoth(status, skill)) return;
        uint16_t want = 0;
        if (!StaminaIndex(status, want)) return;
        const uint16_t spirit = SpiritIndex(status);

        for (const char* key : { kSkillKey_Sprint, kSkillKey_Climb, kSkillKey_Swim,
                                 kSkillKey_Glide, kSkillKey_Horse, kSkillKey_Gallop,
                                 "Skill_CrowSuperDash" })
        {
            const int row = tables::RowByKey(skill, key);
            if (row < 0) continue;
            const uintptr_t rec = tables::Def(skill, static_cast<uint32_t>(row));
            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l) || !l.size) continue;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    const uintptr_t e = EntryAt(l, i);
                    uint16_t idx = 0;
                    uint64_t raw = 0;
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx)) continue;
                    if (idx != want && (spirit == 0xFFFF || idx != spirit)) continue;
                    mem::Read64(e + kOff_URS_VaryStatAmount, &raw);
                    LOG("[verify] %-34s %s %s: %lld", key, idx == want ? "stamina" : "spirit ", when,
                        static_cast<long long>(raw));
                }
            }
        }
    }

    void SurveyCategories()
    {
        Table status, skill;
        if (!OpenBoth(status, skill)) return;
        uint16_t want = 0;
        if (!StaminaIndex(status, want)) return;
        const uint16_t spirit = SpiritIndex(status);

        LOG("[survey] every skill that spends stamina or Spirit, with the fields that might separate movement from "
            "combat. cool is _cooltime, apply _applyType, dmg _damageType, ui _uiType, alert "
            "_isNoAlert, lowres _allowSkillWithLowResource, maxlv _maxLevel, then the entry's own "
            "statType and isRegen.");
        int n = 0;
        for (uint32_t r = 0; r < skill.rows; ++r)
        {
            const uintptr_t rec = tables::Def(skill, r);
            if (!rec) continue;
            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l) || !l.size) continue;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    const uintptr_t e = EntryAt(l, i);
                    uint16_t idx = 0;
                    uint64_t raw = 0;
                    uint8_t statType = 0, regen = 0;
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx)) continue;
                    if (idx != want && (spirit == 0xFFFF || idx != spirit)) continue;
                    const char* stat = idx == want ? "Stamina" : "Spirit ";
                    mem::Read64(e + kOff_URS_VaryStatAmount, &raw);
                    mem::Read8(e + kOff_URS_StatType, &statType);
                    mem::Read8(e + kOff_URS_IsRegen, &regen);

                    uint32_t cool = 0, maxlv = 0;
                    uint8_t apply = 0, dmg = 0, ui = 0, alert = 0, lowres = 0, uiAllowed = 0;
                    mem::Read32(rec + kOff_Skill_Cooltime, &cool);
                    mem::Read32(rec + kOff_Skill_MaxLevel, &maxlv);
                    mem::Read8(rec + kOff_Skill_ApplyType, &apply);
                    mem::Read8(rec + kOff_Skill_DamageType, &dmg);
                    mem::Read8(rec + kOff_Skill_UiType, &ui);
                    mem::Read8(rec + kOff_Skill_IsNoAlert, &alert);
                    mem::Read8(rec + kOff_Skill_AllowLowRes, &lowres);
                    mem::Read8(rec + kOff_Skill_IsUiAllowed, &uiAllowed);

                    char key[96] = "(no key)";
                    tables::StringKey(skill, r, key, sizeof key);
                    LOG("[survey] %s %-46s amt %-8lld list %02X cool %-6u apply %u dmg %u ui %u alert %u "
                        "lowres %u uiok %u maxlv %u statType %u regen %u", stat, key,
                        static_cast<long long>(raw), off, cool, apply, dmg, ui, alert, lowres,
                        uiAllowed, maxlv, statType, regen);
                    ++n;
                }
            }
        }
        LOG("[survey] %d entries listed.", n);
    }

    bool Probe(bool lastTry)
    {
        Table status, skill;
        if (!OpenBoth(status, skill)) return false;

        // The five movement rows and one that must stay clean. This is the
        // check that the walk is reading what the files describe; a row count
        // over the whole table is not, because the file number counts every
        // packed record naming the Stamina status anywhere, including the two
        // rate references every entry carries.
        struct Row { const char* key; bool stamina; };
        const Row rows[] = {
            { kSkillKey_Sprint, true }, { kSkillKey_Climb, true }, { kSkillKey_Swim, true },
            { kSkillKey_Glide,  true }, { kSkillKey_Horse, true }, { kSkillKey_Control, false },
        };

        // Skill opens with its row count before the rows are filled in, so a
        // missing name this early is a table still loading. See Apply.
        if (!lastTry)
            for (const Row& R : rows)
                if (tables::RowByKey(skill, R.key) < 0) return false;

        uint16_t want = 0;
        if (!StaminaIndex(status, want)) { LOG_ERR("[probe] Stamina is not in statusinfo by hash."); return true; }
        LOG_OK("[probe] statusinfo %u rows, skill %u rows, Stamina _statusIndex %u", status.rows,
               skill.rows, want);

        int agreed = 0, checked = 0;
        for (const Row& R : rows)
        {
            const int row = tables::RowByKey(skill, R.key);
            if (row < 0) { LOG_ERR("[probe] no row keyed %s", R.key); continue; }
            const uintptr_t rec = tables::Def(skill, static_cast<uint32_t>(row));
            bool saw = false;
            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l) || !l.size) continue;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    const uintptr_t e = EntryAt(l, i);
                    uint16_t idx = 0, inc = 0, dec = 0;
                    uint8_t statType = 0, regen = 0;
                    uint64_t raw = 0;
                    mem::Read16(e + kOff_URS_StatusInfo, &idx);
                    mem::Read8 (e + kOff_URS_StatType, &statType);
                    mem::Read8 (e + kOff_URS_IsRegen, &regen);
                    mem::Read64(e + kOff_URS_VaryStatAmount, &raw);
                    mem::Read16(e + kOff_URS_IncreaseStatus, &inc);
                    mem::Read16(e + kOff_URS_DecreaseStatus, &dec);
                    char name[96] = "(no key)";
                    tables::StringKey(status, idx, name, sizeof name);
                    LOG("[probe] %s +0x%02X[%u]: status %u %s, statType %u, isRegen %u, amount %lld, "
                        "inc %u dec %u", R.key, off, i, idx, name, statType, regen,
                        static_cast<long long>(raw), inc, dec);
                    if (idx == want) saw = true;
                }
            }
            ++checked;
            if (saw == R.stamina) ++agreed;
            else LOG_ERR("[probe] %s %s a stamina entry and the files say it %s.", R.key,
                         saw ? "has" : "has none", R.stamina ? "should" : "should not");
        }
        LOG(agreed == checked ? "[probe] all %d named rows agree with the files."
                              : "[probe] only %d of %d named rows agree with the files.", agreed, checked);

        const uint16_t spirit = SpiritIndex(status);
        if (spirit == 0xFFFF)
            LOG_ERR("[probe] no statusinfo row hashes to 0x%08X, so Spirit is not named on this build.",
                    kHash_Spirit);
        else
            LOG_OK("[probe] Spirit _statusIndex %u (the files put it one row after Stamina).", spirit);

        // Every entry on the report rows, whatever status it names. Nothing is
        // checked here; these lines are for reading.
        for (const char* key : kSkillKeys_Report)
        {
            const int row = tables::RowByKey(skill, key);
            if (row < 0) { LOG("[probe] report row %s is not in the table.", key); continue; }
            const uintptr_t rec = tables::Def(skill, static_cast<uint32_t>(row));
            int entries = 0;
            for (unsigned off : kLists)
            {
                List l;
                if (!ReadList(rec, off, l)) { LOG("[probe] %s +0x%02X: list unreadable", key, off); continue; }
                for (unsigned i = 0; i < l.size; ++i, ++entries)
                {
                    const uintptr_t e = EntryAt(l, i);
                    uint16_t idx = 0, inc = 0, dec = 0;
                    uint8_t statType = 0, regen = 0;
                    uint64_t raw = 0;
                    mem::Read16(e + kOff_URS_StatusInfo, &idx);
                    mem::Read8 (e + kOff_URS_StatType, &statType);
                    mem::Read8 (e + kOff_URS_IsRegen, &regen);
                    mem::Read64(e + kOff_URS_VaryStatAmount, &raw);
                    mem::Read16(e + kOff_URS_IncreaseStatus, &inc);
                    mem::Read16(e + kOff_URS_DecreaseStatus, &dec);
                    char name[96] = "(no key)";
                    tables::StringKey(status, idx, name, sizeof name);
                    LOG("[probe] report %s +0x%02X[%u]: status %u %s, statType %u, isRegen %u, amount %lld, "
                        "inc %u dec %u%s", key, off, i, idx, name, statType, regen,
                        static_cast<long long>(raw), inc, dec, raw == 0 ? " (amount reads zero)" : "");
                }
            }
            if (!entries) LOG("[probe] report %s: both lists empty", key);
        }
        return true;
    }
}
