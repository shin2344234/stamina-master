#include "game/tables.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "game/mem.h"
#include "game/signatures.h"

using namespace sm::sig;

namespace
{
    struct TableHunt { const char* name; uintptr_t fn; };

    // The resolver clone that names this table: a `lea r8,[rip+"<name>"]` with
    // the 16-bit-key prologue somewhere above it.
    bool TableVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<TableHunt*>(ctx);
        const uintptr_t str = sm::mem::RipAt(hit, 7);
        char buf[48];
        if (!sm::mem::ReadCString(str, buf, sizeof buf) || strcmp(buf, h->name) != 0) return false;
        for (uintptr_t p = hit; p + kMax_LeaToPrologue > hit && p > sm::mem::Game().base; --p)
            if (sm::mem::MatchAt(p, kSig_TableResolver16)) { h->fn = p; return true; }
        return false;
    }

    uintptr_t GlobalFor(const char* name)
    {
        TableHunt h{ name, 0 };
        sm::mem::FindIf(kSig_LeaR8Rip, TableVisit, &h);
        if (!h.fn) return 0;
        const uintptr_t g = sm::mem::RipAt(h.fn + kOff_TableResolver_MovGlobal, 7);
        return sm::mem::InImage(g) ? g : 0;
    }

    // A def offset is only usable if it yields string keys. Try both known
    // positions of the def array and keep the one that reads.
    bool PickDefsOffset(sm::tables::Table& t)
    {
        const unsigned tryOffs[2] = { kOff_Table_DefsA, kOff_Table_DefsB };
        for (unsigned o : tryOffs)
        {
            t.defsOff = o;
            int good = 0;
            for (uint32_t r = 0; r < t.rows && r < 8; ++r)
            {
                const uintptr_t def = sm::tables::Def(t, r);
                char key[64];
                if (def && sm::mem::ReadEngineString(def + kOff_Def_StringKey, key, sizeof key) && strlen(key) >= 2)
                    ++good;
            }
            if (good >= 2) return true;
        }
        t.defsOff = 0;
        return false;
    }

    // Row count and def array, given the table object. Shared by both routes.
    bool Finish(sm::tables::Table& out)
    {
        if (!out.object) return false;
        if (!sm::mem::Read32(out.object + kOff_Table_Count, &out.rows) || !out.rows || out.rows > 0x40000)
            return false;
        return PickDefsOffset(out);
    }
}

namespace sm::tables
{
    bool Resolve(const char* name, Table& out)
    {
        const uintptr_t global = out.global ? out.global : GlobalFor(name);
        out = Table{};
        out.global = global;
        if (!out.global) return false;
        if (!mem::ReadPtr(out.global, &out.object) || !out.object) return false;
        return Finish(out);
    }

    bool ResolveByManager(const char* decoratedName, Table& out)
    {
        // The vtable set does not move, so it is found once and kept. The
        // global holding the manager can be null for the first seconds of a
        // session, which is the case worth retrying.
        const uintptr_t globalWas = out.global;
        out = Table{};
        out.viaRtti = true;
        out.global = globalWas;

        if (!out.global)
        {
            uintptr_t vts[8] = {};
            const int n = mem::FindVtablesByName(decoratedName, vts, 8);
            if (n <= 0) return false;
            out.global = mem::FindGlobalHoldingVtable(vts, n);
            if (!out.global) return false;
        }
        if (!mem::ReadPtr(out.global, &out.object) || !out.object) return false;
        return Finish(out);
    }

    uintptr_t Def(const Table& t, uint32_t row)
    {
        uintptr_t defs = 0, def = 0;
        if (!t.object || row >= t.rows || !t.defsOff) return 0;
        if (!mem::ReadPtr(t.object + t.defsOff, &defs)) return 0;
        if (!mem::ReadPtr(defs + 8ull * row, &def)) return 0;
        return def;
    }

    bool StringKey(const Table& t, uint32_t row, char* out, size_t n)
    {
        const uintptr_t def = Def(t, row);
        if (out && n) out[0] = 0;
        return def && mem::ReadEngineString(def + kOff_Def_StringKey, out, n) && out[0];
    }

    int RowByKey(const Table& t, const char* key)
    {
        char buf[128];
        for (uint32_t r = 0; r < t.rows; ++r)
            if (StringKey(t, r, buf, sizeof buf) && strcmp(buf, key) == 0)
                return static_cast<int>(r);
        return -1;
    }

    void Copy(const Table& t, std::vector<Rec>& out)
    {
        out.clear();
        out.reserve(t.rows);
        const unsigned sizes[] = { kDefScanBytes, 0x400, 0x200, 0x100, 0x80, 0x40 };
        for (uint32_t r = 0; r < t.rows; ++r)
        {
            const uintptr_t def = Def(t, r);
            if (!def) continue;
            Rec rec;
            rec.row = r;
            for (unsigned s : sizes)
                if (mem::ReadBytes(def, rec.b, s)) { rec.len = s; break; }
            if (rec.len) out.push_back(rec);
        }
    }

    int FindU8Offset(const std::vector<Rec>& recs, uint8_t value, uint32_t wantCount)
    {
        for (unsigned off = 0; off < kDefScanBytes; ++off)
        {
            uint32_t hits = 0;
            for (const Rec& r : recs)
                if (off < r.len && r.b[off] == value) ++hits;
            if (hits == wantCount) return static_cast<int>(off);
        }
        return -1;
    }

    // Byte steps, not dword steps. These records are packed: a variable-length
    // string sits inline ahead of the numeric fields, so a field is under no
    // obligation to land on a multiple of four and an aligned scan walks past
    // most of them.
    int FindU32Offset(const std::vector<Rec>& recs, uint32_t value, uint32_t wantCount)
    {
        for (unsigned off = 0; off + 4 <= kDefScanBytes; ++off)
        {
            uint32_t hits = 0;
            for (const Rec& r : recs)
            {
                if (off + 4 > r.len) continue;
                uint32_t v = 0;
                memcpy(&v, r.b + off, sizeof v);
                if (v == value) ++hits;
            }
            if (hits == wantCount) return static_cast<int>(off);
        }
        return -1;
    }

    int FindF32Offset(const std::vector<Rec>& recs, float value, uint32_t wantCount)
    {
        uint32_t want = 0;
        memcpy(&want, &value, sizeof want);
        return FindU32Offset(recs, want, wantCount);
    }

    void U32Offsets(const std::vector<Rec>& recs, uint32_t value, std::vector<OffsetHit>& out)
    {
        out.clear();
        for (unsigned off = 0; off + 4 <= kDefScanBytes; ++off)
        {
            uint32_t hits = 0;
            for (const Rec& r : recs)
            {
                if (off + 4 > r.len) continue;
                uint32_t v = 0;
                memcpy(&v, r.b + off, sizeof v);
                if (v == value) ++hits;
            }
            if (hits) out.push_back({ off, hits });
        }
    }

    uint32_t RowsContainingU32(const std::vector<Rec>& recs, uint32_t value)
    {
        uint32_t rows = 0;
        for (const Rec& r : recs)
        {
            for (unsigned off = 0; off + 4 <= r.len; ++off)
            {
                uint32_t v = 0;
                memcpy(&v, r.b + off, sizeof v);
                if (v == value) { ++rows; break; }
            }
        }
        return rows;
    }

    unsigned RecordStride(const Table& t, uint32_t sample)
    {
        // Small fixed histogram: a def object is a few hundred bytes and a
        // stride outside that range is a pool boundary, not the record size.
        struct Bucket { unsigned gap; uint32_t n; } seen[16] = {};
        int used = 0;
        uintptr_t prev = Def(t, 0);
        for (uint32_t r = 1; r < t.rows && r < sample; ++r)
        {
            const uintptr_t cur = Def(t, r);
            if (!prev || !cur) { prev = cur; continue; }
            const uintptr_t gap = cur - prev;
            prev = cur;
            if (gap == 0 || gap > 0x2000) continue;
            int i = 0;
            for (; i < used; ++i) if (seen[i].gap == gap) { ++seen[i].n; break; }
            if (i == used && used < 16) { seen[used].gap = static_cast<unsigned>(gap); seen[used].n = 1; ++used; }
        }
        unsigned best = 0; uint32_t bestN = 0;
        for (int i = 0; i < used; ++i) if (seen[i].n > bestN) { bestN = seen[i].n; best = seen[i].gap; }
        return bestN >= 4 ? best : 0;
    }

    bool WriteF32(uintptr_t at, float v)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, PAGE_READWRITE, &old)) return false;
        __try { *reinterpret_cast<float*>(at) = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &old); return false; }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &tmp);
        return true;
    }

    bool WriteI64(uintptr_t at, int64_t v)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, PAGE_READWRITE, &old)) return false;
        __try { *reinterpret_cast<int64_t*>(at) = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &old); return false; }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &tmp);
        return true;
    }

    bool WriteU32(uintptr_t at, uint32_t v)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, PAGE_READWRITE, &old)) return false;
        __try { *reinterpret_cast<uint32_t*>(at) = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &old); return false; }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &tmp);
        return true;
    }
}
