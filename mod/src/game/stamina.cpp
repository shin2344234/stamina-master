#include "game/stamina.h"

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/mem.h"
#include "game/signatures.h"
#include "game/tables.h"

using namespace us::sig;
using us::tables::Table;

namespace
{
    struct Slot { Table byName, byRtti; };
    Slot g_statusSlot, g_skillSlot;

    bool Open(const char* name, const char* rtti, Slot& s, Table& out)
    {
        if (name && us::tables::Resolve(name, s.byName)) { out = s.byName; return true; }
        if (rtti && us::tables::ResolveByManager(rtti, s.byRtti)) { out = s.byRtti; return true; }
        return false;
    }

    bool OpenBoth(Table& status, Table& skill)
    {
        return Open(kStr_StatusTable, kRtti_StatusManager, g_statusSlot, status) &&
               Open(nullptr, kRtti_SkillManager, g_skillSlot, skill);
    }

    // {items, size, capacity}, as the reader at RVA 0x01527950 writes it.
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
        if (!us::mem::Read64(rec + off + kOff_List_Items, &p)) return false;
        if (!us::mem::Read32(rec + off + kOff_List_Size, &out.size)) return false;
        if (!us::mem::Read32(rec + off + kOff_List_Cap, &out.cap)) return false;
        if (!out.size) return true;                       // empty is a real answer
        if (out.size > 64 || out.cap < out.size) return false;
        if (!us::mem::Plausible(static_cast<uintptr_t>(p)) ||
            !us::mem::Readable(static_cast<uintptr_t>(p), 1ull * out.size * kRec_UseResourceStatBytes))
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
    bool StaminaIndex(const Table& status, uint16_t& out)
    {
        for (uint32_t r = 0; r < status.rows; ++r)
        {
            uint32_t h = 0;
            const uintptr_t def = us::tables::Def(status, r);
            if (!def || !us::mem::Read32(def + kOff_Status_KeyHash, &h) || h != kHash_Stamina) continue;
            uint32_t idx = 0;
            if (!us::mem::Read32(def + kOff_Status_Index, &idx) || idx > 0xFFFF) return false;
            out = static_cast<uint16_t>(idx);
            return true;
        }
        return false;
    }

    const unsigned kLists[2] = { kOff_Skill_UseResourceStatList, kOff_Skill_UseDriverResourceStatList };
}

namespace us::stamina
{
    int Apply(const Scale& scale, bool verbose)
    {
        const struct { int v; const char* name; } settings[] = {
            { scale.oneOff,     "UsePercent" },
            { scale.continuous, "ContinuousPercent" },
            { scale.mount,      "MountPercent" },
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
            scale.mountRegen == 100)
        {
            LOG("[stamina] every setting is 100, so the game's own numbers stand and nothing is "
                "written.");
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

        int changed = 0, skippedPositive = 0, failed = 0, shown = 0;
        int byKind[4] = {};            // one-off, continuous, mount, mount regen
        static const char* kKind[4] = { "one-off", "continuous", "mount", "mount regen" };
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
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx) || idx != want) continue;
                    if (!mem::Read64(e + kOff_URS_VaryStatAmount, &raw)) continue;
                    mem::Read8(e + kOff_URS_IsRegen, &regen);

                    const int64_t before = static_cast<int64_t>(raw);

                    // Spending is negative. A positive amount gives stamina
                    // back, and the only one of those this mod touches is a
                    // mounted regen rate, because that is what decides whether
                    // a horse tires. Every other give-back is left alone, so
                    // food and rest restore what they did.
                    int kind, percent;
                    if (before > 0)
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
                    if (verbose && shown < 400)
                    {
                        char key[96] = "(no key)";
                        tables::StringKey(skill, r, key, sizeof key);
                        LOG("[stamina]   %-44s %-10s %lld -> %lld", key, kKind[kind],
                            static_cast<long long>(before), static_cast<long long>(after));
                        ++shown;
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
               "%d mounted at %d%%, %d mounted regen at %d%%.%s", changed, changed == 1 ? "" : "s",
               byKind[0], scale.oneOff, byKind[1], scale.continuous, byKind[2], scale.mount,
               byKind[3], scale.mountRegen,
               failed ? " Some writes were refused; see above." : "");
        if (skippedPositive)
            LOG("[stamina] %d entr%s carried a positive amount and %s left alone. Those give stamina "
                "rather than spend it, so food and rest still restore what they did.", skippedPositive,
                skippedPositive == 1 ? "y" : "ies", skippedPositive == 1 ? "was" : "were");
        if (!verbose)
            LOG("[stamina] Set Probe=1 in the ini for a line per skill changed.");
        return changed;
    }

    void Verify(const char* when)
    {
        Table status, skill;
        if (!OpenBoth(status, skill)) return;
        uint16_t want = 0;
        if (!StaminaIndex(status, want)) return;

        for (const char* key : { kSkillKey_Sprint, kSkillKey_Climb, kSkillKey_Swim,
                                 kSkillKey_Glide, kSkillKey_Horse, kSkillKey_Gallop })
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
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx) || idx != want) continue;
                    mem::Read64(e + kOff_URS_VaryStatAmount, &raw);
                    LOG("[verify] %-34s %s: %lld", key, when, static_cast<long long>(raw));
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

        LOG("[survey] every skill that spends stamina, with the fields that might separate movement from "
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
                    if (!mem::Read16(e + kOff_URS_StatusInfo, &idx) || idx != want) continue;
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
                    LOG("[survey] %-46s amt %-8lld list %02X cool %-6u apply %u dmg %u ui %u alert %u "
                        "lowres %u uiok %u maxlv %u statType %u regen %u", key,
                        static_cast<long long>(raw), off, cool, apply, dmg, ui, alert, lowres,
                        uiAllowed, maxlv, statType, regen);
                    ++n;
                }
            }
        }
        LOG("[survey] %d entries listed.", n);
    }

    bool Probe()
    {
        Table status, skill;
        if (!OpenBoth(status, skill)) return false;

        uint16_t want = 0;
        if (!StaminaIndex(status, want)) { LOG_ERR("[probe] Stamina is not in statusinfo by hash."); return true; }
        LOG_OK("[probe] statusinfo %u rows, skill %u rows, Stamina _statusIndex %u", status.rows,
               skill.rows, want);

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
        return true;
    }
}
