#pragma once
#include <cstdint>
#include <vector>

#include "game/signatures.h"

// The game's static info tables. Two ways in, because one is not enough.
//
// Resolve() is Flight Freedom's route: find the resolver clone that names the
// table, read the global it loads, walk the def array. It works for statusinfo
// and every other table the exe reaches through a `lea r8,[rip+"<name>"]`.
//
// ResolveByManager() is for the rest. skill has no resolver clone: its name
// lives behind an accessor stub, so nothing in the image passes "skill" to the
// template and Resolve will never find it. Every table does have a manager
// class with RTTI, and mem's vtable walk finds the global holding it.
namespace us::tables
{
    struct Table
    {
        uintptr_t global = 0;   // address of the pointer the resolver loads
        uintptr_t object = 0;   // the table itself
        uint32_t  rows   = 0;
        unsigned  defsOff = 0;  // whichever of DefsA / DefsB produced string keys
        bool      viaRtti = false;
    };

    // One copied record, and how much of it was readable.
    struct Rec
    {
        uint32_t row = 0;
        unsigned len = 0;
        uint8_t  b[sig::kDefScanBytes] = {};
    };

    // Resolve by table name ("statusinfo"). False while the table is not loaded
    // yet, which is the normal state for the first seconds of a session.
    bool Resolve(const char* name, Table& out);

    // Resolve by the manager's decorated RTTI name
    // (".?AVSkillInfoManager@pa@@"). Same Table on success.
    bool ResolveByManager(const char* decoratedName, Table& out);

    uintptr_t Def(const Table& t, uint32_t row);
    bool StringKey(const Table& t, uint32_t row, char* out, size_t n);

    // Row whose _stringKey is exactly `key`, or -1.
    int RowByKey(const Table& t, const char* key);

    // Copy every record once. Scanning in local memory afterwards turns a
    // million guarded reads into a thousand, which is the difference between a
    // stall the player would notice and a step that is over before the log line
    // after it is written.
    void Copy(const Table& t, std::vector<Rec>& out);

    // The offset in a record where `value` appears on exactly `wantCount` rows,
    // or -1. Naming a field by the count the extracted files predict is the
    // only way to write one safely: a value that correlates with a behaviour is
    // not a name, and a guessed offset lands on some other field.
    int FindU8Offset (const std::vector<Rec>& recs, uint8_t  value, uint32_t wantCount);
    int FindU32Offset(const std::vector<Rec>& recs, uint32_t value, uint32_t wantCount);
    int FindF32Offset(const std::vector<Rec>& recs, float    value, uint32_t wantCount);

    // Every offset at which `value` appears, with how many rows carry it there.
    // Use it when no count is predicted yet and the question is what the record
    // looks like at all.
    struct OffsetHit { unsigned off; uint32_t rows; };
    void U32Offsets(const std::vector<Rec>& recs, uint32_t value, std::vector<OffsetHit>& out);

    // How many rows hold `value` anywhere in the copied bytes.
    uint32_t RowsContainingU32(const std::vector<Rec>& recs, uint32_t value);

    // Guarded writes. The def array is heap data and should already be
    // writable; VirtualProtect is here because "should" is not a thing to find
    // out by faulting on the game's own thread.
    bool WriteF32(uintptr_t at, float v);
    bool WriteU32(uintptr_t at, uint32_t v);
}
