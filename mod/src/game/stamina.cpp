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
    bool g_done = false;

    struct Slot { Table byName, byRtti; };

    bool Open(const char* name, const char* rtti, Slot& s, Table& out)
    {
        if (name && us::tables::Resolve(name, s.byName)) { out = s.byName; return true; }
        if (rtti && us::tables::ResolveByManager(rtti, s.byRtti)) { out = s.byRtti; return true; }
        return false;
    }

    // {items, size, capacity}, exactly as the reader at 0x01527950 writes it.
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

    struct Entry
    {
        uint8_t  statType = 0;
        uint16_t status = 0xFFFF;
        uint8_t  isRegen = 0;
        uint64_t amount = 0;
        uint16_t inc = 0xFFFF, dec = 0xFFFF;
    };

    bool ReadEntry(const List& l, unsigned i, Entry& e)
    {
        const uintptr_t a = l.items + 1ull * i * kRec_UseResourceStatBytes;
        e = Entry{};
        return us::mem::Read8 (a + kOff_URS_StatType,       &e.statType) &&
               us::mem::Read16(a + kOff_URS_StatusInfo,     &e.status)   &&
               us::mem::Read8 (a + kOff_URS_IsRegen,        &e.isRegen)  &&
               us::mem::Read64(a + kOff_URS_VaryStatAmount, &e.amount)   &&
               us::mem::Read16(a + kOff_URS_IncreaseStatus, &e.inc)      &&
               us::mem::Read16(a + kOff_URS_DecreaseStatus, &e.dec);
    }

    void DumpList(const Table& status, const List& l, const char* what, unsigned off)
    {
        if (!l.size) { LOG("[skill]   %s at +0x%02X is empty", what, off); return; }
        LOG("[skill]   %s at +0x%02X: %u of %u at 0x%llX", what, off, l.size, l.cap,
            static_cast<unsigned long long>(l.items));
        for (unsigned i = 0; i < l.size; ++i)
        {
            Entry e;
            if (!ReadEntry(l, i, e)) { LOG_ERR("[skill]     entry %u unreadable", i); continue; }
            char name[96] = "(no key)";
            us::tables::StringKey(status, e.status, name, sizeof name);
            double d = 0;
            memcpy(&d, &e.amount, sizeof d);
            LOG("[skill]     %u: status %u %s, statType %u, isRegen %u, amount i64 %lld / f64 %g, "
                "inc %u dec %u", i, e.status, name, e.statType, e.isRegen,
                static_cast<long long>(e.amount), d, e.inc, e.dec);
        }
    }

    bool HasStatus(const List& l, uint16_t want)
    {
        for (unsigned i = 0; i < l.size; ++i)
        {
            Entry e;
            if (ReadEntry(l, i, e) && e.status == want) return true;
        }
        return false;
    }
}

namespace us::stamina
{
    bool Probe()
    {
        if (g_done) return true;

        static Slot statusSlot, skillSlot;
        Table status, skill;
        if (!Open(kStr_StatusTable, kRtti_StatusManager, statusSlot, status)) return false;
        if (!Open(nullptr, kRtti_SkillManager, skillSlot, skill)) return false;
        g_done = true;

        LOG_OK("[table] statusinfo %u rows, skill %u rows", status.rows, skill.rows);

        int rowStamina = -1;
        for (uint32_t r = 0; r < status.rows; ++r)
        {
            uint32_t h = 0;
            const uintptr_t def = tables::Def(status, r);
            if (def && mem::Read32(def + kOff_Status_KeyHash, &h) && h == kHash_Stamina)
            { rowStamina = static_cast<int>(r); break; }
        }
        if (rowStamina < 0) { LOG_ERR("[stat] Stamina is not in statusinfo by hash."); return true; }

        uint32_t staminaIndex = 0;
        mem::Read32(tables::Def(status, rowStamina) + kOff_Status_Index, &staminaIndex);
        const uint16_t want = static_cast<uint16_t>(staminaIndex);
        LOG_OK("[stat] Stamina is row %d, _statusIndex %u", rowStamina, staminaIndex);

        // Five rows that must carry a stamina cost and one that must not. This
        // is the gate, not a row count: the earlier plan of matching 301 was
        // never sound, because that number counts every packed file record
        // holding the Stamina key anywhere, including the two status
        // references an entry carries at +0x10 and +0x12 and any other field
        // that names a status. The named rows are checkable and that number is
        // not.
        struct Row { const char* key; bool stamina; };
        const Row rows[] = {
            { kSkillKey_Sprint, true }, { kSkillKey_Climb, true }, { kSkillKey_Swim, true },
            { kSkillKey_Glide,  true }, { kSkillKey_Horse, true }, { kSkillKey_Control, false },
        };
        int agreed = 0, checked = 0;
        for (const Row& r : rows)
        {
            const int row = tables::RowByKey(skill, r.key);
            if (row < 0) { LOG_ERR("[skill] no row keyed %s", r.key); continue; }
            const uintptr_t rec = tables::Def(skill, static_cast<uint32_t>(row));
            LOG("[skill] %s row %d at 0x%llX, file says it %s carry a stamina cost", r.key, row,
                static_cast<unsigned long long>(rec), r.stamina ? "should" : "should not");

            bool saw = false;
            const struct { unsigned off; const char* what; } lists[] = {
                { kOff_Skill_UseResourceStatList,       "_useResourceStatList" },
                { kOff_Skill_UseDriverResourceStatList, "_useDriverResourceStatList" },
            };
            for (const auto& L : lists)
            {
                List l;
                if (!ReadList(rec, L.off, l))
                { LOG_ERR("[skill]   %s at +0x%02X did not read", L.what, L.off); continue; }
                DumpList(status, l, L.what, L.off);
                if (l.size && HasStatus(l, want)) saw = true;
            }
            ++checked;
            if (saw == r.stamina) ++agreed;
            else
                LOG_ERR("[skill]   %s %s a stamina entry and the file says it %s. The walk and the files "
                        "disagree about this row.", r.key, saw ? "has" : "has none",
                        r.stamina ? "should" : "should not");
        }
        LOG(agreed == checked ? "[skill] all %d named rows agree with the files."
                              : "[skill] only %d of %d named rows agree with the files.",
            agreed, checked);

        // The whole table, for information and for the size of what a write
        // would touch.
        uint32_t rowsWith = 0, entriesWith = 0, rowsWithAnyList = 0, refused = 0;
        for (uint32_t r = 0; r < skill.rows; ++r)
        {
            const uintptr_t rec = tables::Def(skill, r);
            if (!rec) continue;
            bool any = false, hit = false;
            for (unsigned off : { kOff_Skill_UseResourceStatList, kOff_Skill_UseDriverResourceStatList })
            {
                List l;
                if (!ReadList(rec, off, l)) { ++refused; continue; }
                if (!l.size) continue;
                any = true;
                for (unsigned i = 0; i < l.size; ++i)
                {
                    Entry e;
                    if (ReadEntry(l, i, e) && e.status == want) { hit = true; ++entriesWith; }
                }
            }
            if (any) ++rowsWithAnyList;
            if (hit) ++rowsWith;
        }
        LOG_OK("[skill] %u of %u rows have a resource list at all; %u of those carry a Stamina entry, "
               "%u entries in total. %u list read(s) refused.", rowsWithAnyList, skill.rows, rowsWith,
               entriesWith, refused);
        return true;
    }
}
