#include "game/stamina.h"

#include <cstdio>
#include <cstring>
#include <vector>

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

    // A UseResourceStat list, as the record holds it. Which of the two shapes
    // a list takes is the one thing the loader does not say, so both are
    // measured here rather than assumed.
    struct List
    {
        uintptr_t begin = 0;
        uintptr_t end   = 0;
        uint64_t  span  = 0;
        unsigned  stride = 0;
        unsigned  count = 0;
    };

    // Entries are at least 0x14 bytes (the last named field is a u16 at +0x12)
    // and are almost certainly padded to 4 or 8. Take the stride that divides
    // the span exactly and gives every entry a status index inside the table.
    bool ReadList(uintptr_t rec, unsigned off, uint32_t statusRows, List& out)
    {
        out = List{};
        if (!us::mem::ReadPtr(rec + off, &out.begin)) return false;
        if (!us::mem::ReadPtr(rec + off + 8, &out.end)) return false;
        if (!out.begin || out.end < out.begin) return false;
        out.span = out.end - out.begin;
        if (!out.span || out.span > 0x1000) return false;

        for (unsigned stride : { 0x18u, 0x14u, 0x20u, 0x10u })
        {
            if (out.span % stride) continue;
            const unsigned n = static_cast<unsigned>(out.span / stride);
            if (!n || n > 32) continue;
            bool sane = true;
            for (unsigned i = 0; i < n && sane; ++i)
            {
                uint16_t idx = 0;
                if (!us::mem::Read16(out.begin + 1ull * i * stride + kOff_URS_StatusInfo, &idx) ||
                    idx >= statusRows)
                    sane = false;
            }
            if (!sane) continue;
            out.stride = stride;
            out.count = n;
            return true;
        }
        // Span read but no stride fits: still worth reporting.
        return true;
    }

    // Everything a UseResourceStat says, with the name of whatever status it
    // points at, so the log can be read without a table beside it.
    void DumpEntries(const List& l, const Table& status, const char* tag)
    {
        if (!l.stride)
        {
            LOG_ERR("[%s]   list spans %llu bytes and no entry stride of 0x10 to 0x20 divides it into "
                    "entries whose status index is inside the table. Not walking it.", tag,
                    static_cast<unsigned long long>(l.span));
            return;
        }
        for (unsigned i = 0; i < l.count; ++i)
        {
            const uintptr_t e = l.begin + 1ull * i * l.stride;
            uint16_t idx = 0;
            uint8_t regen = 0;
            uint64_t raw = 0;
            us::mem::Read16(e + kOff_URS_StatusInfo, &idx);
            us::mem::Read8(e + kOff_URS_IsRegen, &regen);
            us::mem::Read64(e + kOff_URS_VaryStatAmount, &raw);

            char name[96] = "(no key)";
            us::tables::StringKey(status, idx, name, sizeof name);

            double asDouble = 0;
            memcpy(&asDouble, &raw, sizeof asDouble);
            LOG("[%s]   entry %u: status %u %s, isRegen %u, _varyStatAmount raw %llu / i64 %lld / f64 %g",
                tag, i, idx, name, regen, static_cast<unsigned long long>(raw),
                static_cast<long long>(raw), asDouble);
        }
    }

    void WalkSkill(const Table& skill, const Table& status, uint32_t row, const char* key,
                   uint16_t staminaIndex, bool expectStamina)
    {
        const uintptr_t rec = us::tables::Def(skill, row);
        if (!rec) return;

        LOG("[skill] %s row %u at 0x%llX, file says it %s carry a stamina cost", key, row,
            static_cast<unsigned long long>(rec), expectStamina ? "should" : "should not");

        const struct { unsigned off; const char* what; } lists[] = {
            { kOff_Skill_UseResourceStatList,       "_useResourceStatList" },
            { kOff_Skill_UseDriverResourceStatList, "_useDriverResourceStatList" },
        };
        bool sawStamina = false;
        for (const auto& L : lists)
        {
            List l;
            if (!ReadList(rec, L.off, status.rows, l) || !l.begin)
            {
                LOG("[skill]   %s at +0x%02X is empty", L.what, L.off);
                continue;
            }
            LOG("[skill]   %s at +0x%02X: 0x%llX..0x%llX, %llu bytes, stride 0x%X, %u entr%s", L.what,
                L.off, static_cast<unsigned long long>(l.begin), static_cast<unsigned long long>(l.end),
                static_cast<unsigned long long>(l.span), l.stride, l.count, l.count == 1 ? "y" : "ies");
            DumpEntries(l, status, "skill");
            for (unsigned i = 0; i < l.count; ++i)
            {
                uint16_t idx = 0;
                if (us::mem::Read16(l.begin + 1ull * i * l.stride + kOff_URS_StatusInfo, &idx) &&
                    idx == staminaIndex)
                    sawStamina = true;
            }
        }
        if (sawStamina != expectStamina)
            LOG_ERR("[skill]   %s %s a stamina entry and the extracted table says it %s. The walk and the "
                    "files disagree about this row.", key, sawStamina ? "has" : "has no",
                    expectStamina ? "should" : "should not");
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

        LOG_OK("[table] statusinfo %u rows, skill %u rows, strides %u and %u", status.rows, skill.rows,
               tables::RecordStride(status), tables::RecordStride(skill));

        // Find the two stamina statuses by the hash the record carries rather
        // than by their string key. The hash is what the game itself matches
        // on, it is four bytes at a known offset, and it was checked offline
        // against hashlittle over the lowercase name before being trusted here.
        int rowStamina = -1, rowRate = -1;
        for (uint32_t r = 0; r < status.rows; ++r)
        {
            uint32_t h = 0;
            const uintptr_t def = tables::Def(status, r);
            if (!def || !mem::Read32(def + kOff_Status_KeyHash, &h)) continue;
            if (h == kHash_Stamina) rowStamina = static_cast<int>(r);
            else if (h == kHash_StaminaUseDown) rowRate = static_cast<int>(r);
        }
        if (rowStamina < 0)
        {
            LOG_ERR("[stat] no statusinfo row hashes to 0x%08X. Without Stamina named there is nothing to "
                    "scale and nothing below will run.", kHash_Stamina);
            return true;
        }

        // _statusIndexXXXXX is what a UseResourceStat stores, so read it rather
        // than assuming it equals the row.
        uint32_t staminaIndex = 0, rateIndex = 0;
        uint8_t staminaPercent = 0, ratePercent = 0;
        mem::Read32(tables::Def(status, rowStamina) + kOff_Status_Index, &staminaIndex);
        mem::Read8(tables::Def(status, rowStamina) + kOff_Status_UsePercent, &staminaPercent);
        LOG_OK("[stat] Stamina: row %d, _statusIndex %u, _usePercent %u", rowStamina, staminaIndex,
               staminaPercent);
        if (staminaIndex != static_cast<uint32_t>(rowStamina))
            LOG("[stat] the status index and the row differ, so the index is what the entries below mean.");
        if (rowRate >= 0)
        {
            mem::Read32(tables::Def(status, rowRate) + kOff_Status_Index, &rateIndex);
            mem::Read8(tables::Def(status, rowRate) + kOff_Status_UsePercent, &ratePercent);
            LOG_OK("[stat] Stamina_UseResourceDecreaseRate: row %d, _statusIndex %u, _usePercent %u",
                   rowRate, rateIndex, ratePercent);
        }

        // The six named rows in detail.
        struct Row { const char* key; bool stamina; };
        const Row rows[] = {
            { kSkillKey_Sprint, true }, { kSkillKey_Climb, true }, { kSkillKey_Swim, true },
            { kSkillKey_Glide,  true }, { kSkillKey_Horse, true }, { kSkillKey_Control, false },
        };
        for (const Row& r : rows)
        {
            const int row = tables::RowByKey(skill, r.key);
            if (row < 0) { LOG_ERR("[skill] no row keyed %s", r.key); continue; }
            WalkSkill(skill, status, static_cast<uint32_t>(row), r.key,
                      static_cast<uint16_t>(staminaIndex), r.stamina);
        }

        // Then the whole table, which is the number that decides whether a
        // write is safe. The extracted files say 301 rows carry the Stamina
        // key; if the walk agrees, it is reading the same lists the files
        // describe and scaling them is a known quantity.
        uint32_t rowsWith = 0, entries = 0, listsRead = 0, listsRefused = 0;
        for (uint32_t r = 0; r < skill.rows; ++r)
        {
            const uintptr_t rec = tables::Def(skill, r);
            if (!rec) continue;
            bool hit = false;
            for (unsigned off : { kOff_Skill_UseResourceStatList, kOff_Skill_UseDriverResourceStatList })
            {
                List l;
                if (!ReadList(rec, off, status.rows, l) || !l.begin) continue;
                if (!l.stride) { ++listsRefused; continue; }
                ++listsRead;
                for (unsigned i = 0; i < l.count; ++i)
                {
                    uint16_t idx = 0;
                    if (!mem::Read16(l.begin + 1ull * i * l.stride + kOff_URS_StatusInfo, &idx)) continue;
                    if (idx == static_cast<uint16_t>(staminaIndex)) { hit = true; ++entries; }
                }
            }
            if (hit) ++rowsWith;
        }
        LOG_OK("[skill] %u of %u rows carry a Stamina entry, %u entries in all, across %u lists read. "
               "%u list(s) were refused for having no stride that fits.", rowsWith, skill.rows, entries,
               listsRead, listsRefused);
        if (rowsWith == kSkillRowsWithStamina)
            LOG_OK("[skill] that is exactly what the extracted table says, so the walk is reading the same "
                   "lists the files describe and the costs can be scaled from here.");
        else
            LOG_ERR("[skill] the extracted table says %u. Until that is explained nothing should be "
                    "written: a count that does not match means the walk is not seeing what the files see.",
                    kSkillRowsWithStamina);
        return true;
    }
}
