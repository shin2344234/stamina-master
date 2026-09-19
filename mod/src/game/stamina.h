#pragma once
#include <cstdint>

// Scaling the stamina cost of everything the player does.
//
// The costs are UseResourceStat entries in the skill table's
// _useResourceStatList, one per skill that spends stamina, each naming the
// Stamina status by its row index and carrying an int64 _varyStatAmount that
// is negative because spending is a negative change. Scaling that number is
// the whole mod. private/FEASIBILITY.md has how it was found.
namespace us::stamina
{
    // Multiply every stamina cost by `percent`/100. 100 changes nothing and is
    // refused as a no-op; 0 removes the cost outright.
    //
    // Returns how many entries were rewritten, 0 when the tables are not
    // loaded yet (worth calling again), or -1 when something was wrong enough
    // that nothing was written. Only negative amounts are touched, so a skill
    // that restores stamina keeps restoring the same amount.
    int Apply(int percent, bool verbose);

    // The research report. Independent of Apply and off by default.
    bool Probe();

    // One line per skill that spends stamina, with the fields that might tell
    // a movement skill from a combat one. Written so the split can be made a
    // setting on the strength of a field the game itself keeps, rather than on
    // a keyword rule over the skill key. Runs under Probe=1 only.
    void SurveyCategories();
}
