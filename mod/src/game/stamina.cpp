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

    void ReportTable(const char* label, const Table& t, uint32_t expectRows)
    {
        LOG_OK("[table] %s at +0x%llX, %u rows, defs at +0x%02X, found by %s", label,
               static_cast<unsigned long long>(us::mem::Rva(t.global)), t.rows, t.defsOff,
               t.viaRtti ? "the manager's RTTI" : "the resolver clone");
        if (t.rows != expectRows)
            LOG_ERR("[table] %s has %u rows and the extracted tables for this build say %u. The data has "
                    "moved since those were generated, so no offset below should be trusted and nothing "
                    "will be written on the strength of it. Regenerate with scripts/ first.",
                    label, t.rows, expectRows);
    }

    // The first bytes of a record, as hex, for reading a layout off the log.
    void DumpRecord(const Table& t, uint32_t row, unsigned bytes)
    {
        const uintptr_t def = us::tables::Def(t, row);
        if (!def) return;
        uint8_t b[128] = {};
        if (bytes > sizeof b) bytes = sizeof b;
        if (!us::mem::ReadBytes(def, b, bytes)) return;
        char line[3 * 32 + 1];
        for (unsigned off = 0; off < bytes; off += 32)
        {
            const unsigned n = (bytes - off < 32) ? bytes - off : 32;
            for (unsigned i = 0; i < n; ++i) snprintf(line + 3 * i, 4, "%02X ", b[off + i]);
            line[3 * n] = 0;
            LOG("[table]     +%03X  %s", off, line);
        }
    }

    // A named row, with its record head. Returns the row or -1, and says which
    // when it fails, because "the table is not the one we think" and "the row
    // was renamed" want different next steps.
    int NamedRow(const Table& t, const char* key, const char* what, bool dump)
    {
        const int row = us::tables::RowByKey(t, key);
        if (row < 0)
        {
            LOG_ERR("[%s] no row keyed %s in %u rows.", what, key, t.rows);
            return -1;
        }
        LOG_OK("[%s] %s is row %d.", what, key, row);
        if (dump) DumpRecord(t, static_cast<uint32_t>(row), 64);
        return row;
    }
}

namespace us::stamina
{
    bool Probe()
    {
        if (g_done) return true;

        // Static so the image scan that finds each global happens once, not on
        // every poll while the tables are still empty.
        static Slot statusSlot, skillSlot;
        Table status, skill;
        const bool haveStatus = Open(kStr_StatusTable, kRtti_StatusManager, statusSlot, status);
        const bool haveSkill  = Open(nullptr, kRtti_SkillManager, skillSlot, skill);
        if (!haveStatus && !haveSkill) return false;
        g_done = true;

        // ---- statusinfo ----------------------------------------------------
        // Plan A rests on Stamina_UseResourceDecreaseRate being a real row the
        // game resolves. If it is not here, plan A is over before it starts.
        if (!haveStatus)
        {
            LOG_ERR("[table] statusinfo answered neither the resolver clone nor %s. Plan A cannot be "
                    "checked this session.", kRtti_StatusManager);
        }
        else
        {
            ReportTable("statusinfo", status, kStatusRows);
            NamedRow(status, kStatusName_Stamina, "stat", true);
            const int down = NamedRow(status, kStatusName_StaminaUseDown, "stat", true);
            if (down < 0)
                LOG_ERR("[stat] %s is in the extracted statusinfo as key %u but the running table does not "
                        "have it. Plan A is off for this build.",
                        kStatusName_StaminaUseDown, kStatusKey_StaminaUseDown);

            // Where each status key sits inside a record, so the key field can
            // be named. Every key is unique, so the count method does not apply
            // and this is the honest substitute: show the offsets and let the
            // reader pick the one that holds the right key on both rows.
            std::vector<Rec> recs;
            tables::Copy(status, recs);
            LOG("[table] copied %zu of %u statusinfo records", recs.size(), status.rows);
            std::vector<tables::OffsetHit> hits;
            for (uint32_t key : { kStatusKey_Stamina, kStatusKey_StaminaUseDown, kStatusKey_Hp })
            {
                tables::U32Offsets(recs, key, hits);
                if (hits.empty())
                {
                    LOG_ERR("[stat] status key %u appears nowhere in any statusinfo record.", key);
                    continue;
                }
                for (const tables::OffsetHit& h : hits)
                    LOG("[stat] status key %u at record +0x%03X on %u row%s", key, h.off, h.rows,
                        h.rows == 1 ? "" : "s");
            }
        }

        // ---- skill ---------------------------------------------------------
        // Plan B rests on reaching the skill table and finding the stamina
        // costs inside a record. skill has no resolver clone, so this is the
        // first time the RTTI route has been asked for anything.
        if (!haveSkill)
        {
            LOG_ERR("[table] skill did not answer %s. The manager vtable or the global holding it was not "
                    "found, so plan B cannot be checked this session. This is the one route skill has: it "
                    "has no resolver clone naming it.", kRtti_SkillManager);
            return true;
        }

        ReportTable("skill", skill, kSkillRows);
        for (const char* key : { kSkillKey_Sprint, kSkillKey_Climb, kSkillKey_Swim,
                                 kSkillKey_Glide, kSkillKey_Horse })
            NamedRow(skill, key, "skill", false);

        std::vector<Rec> recs;
        tables::Copy(skill, recs);
        LOG("[table] copied %zu of %u skill records", recs.size(), skill.rows);

        // A record is copied at a fixed size and the real ones run from about
        // 200 to 1,550 bytes, so a copy reaches past the short ones into
        // whatever the heap put next. An over-count is therefore the expected
        // answer and not a fault; an under-count is the one to worry about,
        // because nothing about a short read can hide a key that is there.
        const uint32_t withStamina = tables::RowsContainingU32(recs, kStatusKey_Stamina);
        if (withStamina == kSkillRowsWithStamina)
            LOG_OK("[skill] %u rows carry the Stamina status key, exactly what the extracted table says. "
                   "The record layout is the one the research was written against.", withStamina);
        else if (withStamina > kSkillRowsWithStamina)
            LOG("[skill] %u rows carry the Stamina status key against the extracted table's %u. Each record "
                "is copied at a fixed %u bytes and the real ones are shorter than that, so the extra rows "
                "are reads that ran into the next record. Use the offsets below, not this count.",
                withStamina, kSkillRowsWithStamina, kDefScanBytes);
        else
            LOG_ERR("[skill] only %u rows carry the Stamina status key and the extracted table says %u. A "
                    "short copy cannot hide a key that is there, so the layout has moved or this is not the "
                    "skill table. Nothing is written on this.", withStamina, kSkillRowsWithStamina);

        // Where inside a record the cost sits. A list field is a pointer and a
        // count rather than an inline value, so the key may well be out of
        // reach of a fixed-size copy; that is worth knowing either way and the
        // log says which it is.
        std::vector<tables::OffsetHit> hits;
        tables::U32Offsets(recs, kStatusKey_Stamina, hits);
        LOG("[skill] the Stamina status key appears at %zu distinct record offsets", hits.size());
        int shown = 0;
        for (const tables::OffsetHit& h : hits)
        {
            if (shown++ >= 24) { LOG("[skill]   ... %zu more", hits.size() - 24); break; }
            LOG("[skill]   +0x%03X on %u row%s", h.off, h.rows, h.rows == 1 ? "" : "s");
        }

        // The five named rows in full, which is what a layout gets read off.
        for (const char* key : { kSkillKey_Sprint, kSkillKey_Climb, kSkillKey_Horse })
        {
            const int row = tables::RowByKey(skill, key);
            if (row < 0) continue;
            LOG("[skill] %s record head:", key);
            DumpRecord(skill, static_cast<uint32_t>(row), 128);
        }
        return true;
    }
}
