#include "game/stamina.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/mem.h"
#include "game/signatures.h"
#include "game/tables.h"

using namespace us::sig;
using us::tables::Rec;
using us::tables::Table;

namespace
{
    bool g_done = false;

    // Both routes, in order, with a line saying which answered. statusinfo has
    // a resolver clone and skill does not, so a table that answers only through
    // RTTI is expected rather than a warning.
    //
    // Each route keeps its own Table, because each caches a different global:
    // the resolver clone's is the pointer the template loads and the RTTI
    // route's is whichever global holds the manager. Sharing one across both,
    // which the first draft did, hands the second route the first route's
    // address on every retry and mislabels whichever answers.
    struct Slot { Table byName, byRtti; };

    bool Open(const char* name, const char* rtti, Slot& s, Table& out)
    {
        if (name && us::tables::Resolve(name, s.byName)) { out = s.byName; return true; }
        if (rtti && us::tables::ResolveByManager(rtti, s.byRtti)) { out = s.byRtti; return true; }
        return false;
    }

    // Telling a pointer from a pair of small numbers is the whole difficulty in
    // reading one of these records off a hex dump. The defs sit in a high arena
    // and every pointer in them shares its top dword, but that dword is not a
    // constant to test against: two sessions twenty seconds apart put it at
    // 0x438 and then 0x38D. Ask the process instead.
    bool LooksLikePointer(uint64_t v)
    {
        return us::mem::Plausible(static_cast<uintptr_t>(v)) &&
               us::mem::Readable(static_cast<uintptr_t>(v), 8);
    }

    // A record, 16 bytes to a line, with each dword read three ways. Floats are
    // only printed when they are the sort of number a designer would type.
    void Dump(uintptr_t def, unsigned bytes, const char* tag)
    {
        uint8_t b[512] = {};
        if (bytes > sizeof b) bytes = sizeof b;
        if (!def || !us::mem::ReadBytes(def, b, bytes))
        {
            LOG_ERR("[%s] could not read %u bytes at 0x%llX", tag, bytes,
                    static_cast<unsigned long long>(def));
            return;
        }
        for (unsigned off = 0; off < bytes; off += 16)
        {
            char hex[16 * 3 + 1];
            for (unsigned i = 0; i < 16; ++i) snprintf(hex + 3 * i, 4, "%02X ", b[off + i]);
            hex[48] = 0;

            char note[160] = {};
            unsigned n = 0;
            for (unsigned i = 0; i < 16; i += 4)
            {
                uint32_t u = 0; float f = 0;
                memcpy(&u, b + off + i, 4);
                memcpy(&f, b + off + i, 4);
                if (u == 0) continue;
                if (f > 0.0001f && f < 1000000.0f)
                    n += snprintf(note + n, sizeof note - n, "+%X=%u/%g ", off + i, u, f);
                else
                    n += snprintf(note + n, sizeof note - n, "+%X=%u ", off + i, u);
                if (n >= sizeof note - 24) break;
            }
            LOG("[%s]   +%03X  %s %s", tag, off, hex, note);
        }
    }

    // Everywhere inside `blob` that holds `needle`, as a u32 or as a u64.
    int FindIn(const uint8_t* blob, unsigned len, uint64_t needle, bool wide, unsigned* firstAt)
    {
        const unsigned w = wide ? 8u : 4u;
        int hits = 0;
        for (unsigned off = 0; off + w <= len; ++off)
        {
            uint64_t v = 0;
            memcpy(&v, blob + off, w);
            if (v != needle) continue;
            if (!hits && firstAt) *firstAt = off;
            ++hits;
        }
        return hits;
    }

    // The question this pass exists to answer. A skill def is a fixed-size
    // object and _useResourceStatList is a list, so the cost is not in the
    // record: it is behind a pointer in it. Walk every pointer-shaped qword in
    // the record, read what it points at, and say which one leads to the
    // Stamina status.
    //
    // Two needles, because either could be how a UseResourceStat names its
    // status: the raw key 1000026, or the address of statusinfo's own def for
    // that row.
    void WalkPointers(const Table& skill, uint32_t row, const char* key,
                      unsigned stride, uintptr_t staminaDef, bool expectStamina)
    {
        const uintptr_t def = us::tables::Def(skill, row);
        if (!def) return;

        uint8_t rec[1024] = {};
        const unsigned len = (stride && stride <= sizeof rec) ? stride : 0x140;
        if (!us::mem::ReadBytes(def, rec, len))
        {
            LOG_ERR("[walk] %s: could not read its %u byte record", key, len);
            return;
        }

        LOG("[walk] %s row %u at 0x%llX, %u byte record, %s carry a stamina cost", key, row,
            static_cast<unsigned long long>(def), len,
            expectStamina ? "the file says it should" : "the file says it should not");

        int leads = 0;
        for (unsigned off = 0; off + 8 <= len; off += 8)
        {
            uint64_t p = 0;
            memcpy(&p, rec + off, 8);
            if (!LooksLikePointer(p)) continue;

            uint8_t blob[512] = {};
            unsigned got = 0;
            for (unsigned want : { 512u, 256u, 128u, 64u, 32u })
                if (us::mem::ReadBytes(static_cast<uintptr_t>(p), blob, want)) { got = want; break; }
            if (!got) continue;

            unsigned atKey = 0, atDef = 0;
            const int byKey = FindIn(blob, got, kStatusKey_Stamina, false, &atKey);
            const int byDef = staminaDef ? FindIn(blob, got, staminaDef, true, &atDef) : 0;
            if (!byKey && !byDef) continue;

            ++leads;
            LOG_OK("[walk]   record +0x%03X -> 0x%llX%s%s", off, static_cast<unsigned long long>(p),
                   byKey ? "  holds the Stamina key" : "",
                   byDef ? "  holds statusinfo's Stamina def" : "");
            if (byKey) LOG("[walk]     key %u first at +0x%03X, %d time%s in %u bytes",
                           kStatusKey_Stamina, atKey, byKey, byKey == 1 ? "" : "s", got);
            if (byDef) LOG("[walk]     def pointer first at +0x%03X, %d time%s in %u bytes",
                           atDef, byDef, byDef == 1 ? "" : "s", got);
            Dump(static_cast<uintptr_t>(p), got < 128 ? got : 128, "walk");
        }

        if (!leads)
            LOG("[walk]   no pointer in the record leads to the Stamina status within one hop.");
        else if (!expectStamina)
            LOG_ERR("[walk]   %s is the control and should have led nowhere. A needle that turns up on "
                    "every row is not the cost list.", key);
    }
}

namespace us::stamina
{
    bool Probe()
    {
        if (g_done) return true;

        static Slot statusSlot, skillSlot;
        Table status, skill;
        const bool haveStatus = Open(kStr_StatusTable, kRtti_StatusManager, statusSlot, status);
        const bool haveSkill  = Open(nullptr, kRtti_SkillManager, skillSlot, skill);
        if (!haveStatus || !haveSkill) return false;
        g_done = true;

        LOG_OK("[table] statusinfo %u rows, skill %u rows", status.rows, skill.rows);

        const unsigned statusStride = tables::RecordStride(status);
        const unsigned skillStride  = tables::RecordStride(skill);
        LOG("[table] record stride: statusinfo %u bytes, skill %u bytes. The first pass read this off the "
            "way a fixed-size copy wrapped into the next record; this is the defs' own spacing.",
            statusStride, skillStride);

        // ---- statusinfo ----------------------------------------------------
        // The whole record for both stamina rows. The first pass settled that
        // +0x00 is the row key and +0x08 the string; what is wanted now is the
        // cap and the percent flag, which decide what writing the rate means.
        const int rowStamina = tables::RowByKey(status, kStatusName_Stamina);
        const int rowRate    = tables::RowByKey(status, kStatusName_StaminaUseDown);
        uintptr_t staminaDef = 0;
        if (rowStamina >= 0)
        {
            staminaDef = tables::Def(status, static_cast<uint32_t>(rowStamina));
            LOG_OK("[stat] %s is row %d, def at 0x%llX", kStatusName_Stamina, rowStamina,
                   static_cast<unsigned long long>(staminaDef));
            Dump(staminaDef, statusStride ? statusStride : 0xE0, "stat");
        }
        if (rowRate >= 0)
        {
            const uintptr_t d = tables::Def(status, static_cast<uint32_t>(rowRate));
            LOG_OK("[stat] %s is row %d, def at 0x%llX", kStatusName_StaminaUseDown, rowRate,
                   static_cast<unsigned long long>(d));
            Dump(d, statusStride ? statusStride : 0xE0, "stat");
        }
        else
        {
            LOG_ERR("[stat] %s is not in the running statusinfo. Writing it is off the table.",
                    kStatusName_StaminaUseDown);
        }

        // ---- skill ---------------------------------------------------------
        // Five rows the file says carry a stamina cost, and one it says does
        // not. The control is the point: a pointer that leads to the Stamina
        // status on Skill_Looting as well is not the cost list, it is something
        // every skill shares.
        struct Row { const char* key; bool stamina; };
        const Row rows[] = {
            { kSkillKey_Sprint, true },
            { kSkillKey_Climb,  true },
            { kSkillKey_Swim,   true },
            { kSkillKey_Glide,  true },
            { kSkillKey_Horse,  true },
            { kSkillKey_Control, false },
        };
        for (const Row& r : rows)
        {
            const int row = tables::RowByKey(skill, r.key);
            if (row < 0) { LOG_ERR("[walk] no skill row keyed %s", r.key); continue; }
            WalkPointers(skill, static_cast<uint32_t>(row), r.key, skillStride, staminaDef, r.stamina);
        }
        return true;
    }
}
