#pragma once
#include <cstdint>

// Scaling the stamina cost of everything the player does.
//
// The costs are UseResourceStat entries in the skill table's
// _useResourceStatList, one per skill that spends stamina, each naming the
// Stamina status by its row index and carrying an int64 _varyStatAmount that
// is negative because spending is a negative change. Scaling that number is
// the whole mod. private/FEASIBILITY.md has how it was found.
namespace sm::stamina
{
    // What to keep of each kind of cost, as a percentage. 100 leaves a kind
    // alone and 0 removes its cost outright.
    //
    // The three kinds are the ones the game's own data supports, which is not
    // the movement-against-combat split anyone would reach for first. Nothing
    // in a skill record separates climbing from attacking: _cooltime,
    // _applyType, _damageType, _uiType, _isNoAlert, _allowSkillWithLowResource,
    // _isUiUseAllowed and _maxLevel were all checked over every skill that
    // spends stamina, along with the entry's statType and the nine-row
    // skillgroupinfo table, and climbing shares every value with attacking.
    //
    //   mount       _applyType is 1. Exactly the mounted skills, 15 of them.
    //   continuous  _isRegen is 1 on the entry: a drain that runs while you
    //               hold it. Sprinting, climbing, swimming, the glider, the
    //               rocket pack, and also channelled attacks, because those
    //               are continuous too. 61.
    //   one-off     everything else, charged once per use. A roll, a jump,
    //               each swing. 130.
    struct Scale
    {
        int oneOff     = 100;
        int continuous = 100;
        int mount      = 100;

        // Mounted skills work the other way round and nothing above reaches
        // them. A horse's gait is a regen entry whose rate falls as it speeds
        // up: +100000 standing, +40000 at a canter, +9000 at a full gallop.
        // Its stamina falls at speed because something drains faster than
        // 9000 a tick, and zeroing the one negative mounted cost,
        // Skill_HorseStamina at -10000, did not stop it. Raising the regen
        // does not need to know where the rest of the drain comes from.
        //
        // A percentage of the game's own rate, 100 to 10000. 100 leaves it
        // alone. This is the only setting that multiplies a positive amount,
        // and it only touches mounted skills.
        int mountRegen = 100;
    };

    // Returns how many entries were rewritten, 0 when the tables are not
    // loaded yet (worth calling again), or -1 when something was wrong enough
    // that nothing was written. Only negative amounts are touched, so a skill
    // that restores stamina keeps restoring the same amount.
    int Apply(const Scale& scale, bool verbose);

    // The research report. Independent of Apply and off by default.
    bool Probe();

    // One line per skill that spends stamina, with the fields that might tell
    // a movement skill from a combat one. Written so the split can be made a
    // setting on the strength of a field the game itself keeps, rather than on
    // a keyword rule over the skill key. Runs under Probe=1 only.
    void SurveyCategories();

    // Read the five named movement costs back and say what they hold now.
    //
    // Apply already reads each write back at the moment it makes it, which
    // proves the store landed and nothing more. This runs later, on a timer,
    // and answers a different question: whether the value is still there once
    // the player is in the world. If it is, and stamina still drains at 0, the
    // skill table is not the number the game spends against and the search has
    // to move to the action charts, which is where the Nexus mod did its work.
    void Verify(const char* when);
}
