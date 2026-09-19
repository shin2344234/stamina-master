#pragma once
#include <cstdint>

// Stamina work, and for this build the probe that has to come before it.
//
// Nothing in here writes to the game yet. The feasibility pass found two ways
// to build the mod and could not choose between them from the files alone, so
// this reports what the running game actually holds and the choice is made off
// the log. See private/FEASIBILITY.md.
namespace us::stamina
{
    // Run the whole report once. Returns true when it has run; call again each
    // second until it does, because the tables are not populated at load time.
    // Reports what it could not do rather than falling silent.
    bool Probe();
}
