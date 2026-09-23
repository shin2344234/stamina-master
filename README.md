# Stamina Master

A stamina plugin for Crimson Desert 2.03.02 (exe 1.0.0.2976). It reads the
skill table by its layout and not by address, so 2.03.00 runs it too.

It multiplies the stamina cost of what you do, so sprinting, climbing,
swimming, gliding and riding cost a fraction of what the game charges. How much
of a fraction is three numbers in an ini rather than a choice between five
downloads, and no game file is touched.

Downloads are on [Nexus](https://www.nexusmods.com/crimsondesert/mods/3549)
and under [releases](https://github.com/shin2344234/stamina-master/releases).

## Why it is a plugin

Nexus mod 107, Infinite Stamina by PhorgeForge, does the same job by replacing
game data: an archive that drops a `0036` folder into the game root and
overwrites `meta\0.papgt`. That was last updated in April 2026 and two game
patches have landed since.

On a current install its instructions are also actively harmful. Vanilla
`0.papgt` already lists slots `0036` through `0040`, so the folder it tells you
to create is supported in stock, and the live file on a DMM install carries two
extra pack groups, `dmmgen` and `dmmsa`, that overwriting it would remove.

## Settings

    UsePercent=0           130 costs charged once per use: a roll, a jump, a swing
    ContinuousPercent=0     61 drains that run while held: sprint, climb, swim, glide
    MountPercent=0          15 things you do while mounted
    MountRegenPercent=1000  how fast a horse recovers, 100 to 10000
    Probe=0                 write the research report and a line per skill changed

The first three are a percentage of the game's own cost, so 100 leaves that
kind alone and 0 removes it outright. A value outside 0 to 100 refuses the
whole write, including whichever settings were in range.

`MountRegenPercent` goes the other way, because a horse does. Its gait is a
recovery rate that falls as it speeds up, and it tires at speed because
something drains faster than that rate. Raising the recovery is what stops it,
and 1000 puts a full gallop at roughly what standing still gives. Below 100 is
refused.

Of the 256 entries that name Stamina, 206 are costs and get scaled, 26 are
already zero, and 24 give stamina back. Twelve of those give-backs are a
horse's gait and `MountRegenPercent` raises them. The other twelve are your own
recovery while standing, walking, guarding or hanging on a ledge, and nothing
here touches them.

## Why those three kinds and not movement against combat

Because the game does not record whether a skill is for travelling or for
fighting. Every skill that spends stamina was checked on `_cooltime`,
`_applyType`, `_damageType`, `_uiType`, `_isNoAlert`,
`_allowSkillWithLowResource`, `_isUiUseAllowed` and `_maxLevel`, on the
entry's own `statType` and `isRegen`, and against the nine-row `skillgroupinfo`
table. Climbing shares every value with attacking, and `skillgroupinfo` turned
out to be weapon mastery: `Skill_CharacterClimb` belongs to a group and
`Skill_ContinuousAttack_10` does not.

Two things the data does separate cleanly:

`_applyType` of 1 is exactly the mounted skills and nothing else.

`isRegen` on the cost says whether it is charged once or drains while you hold
it. That is about how a cost is billed rather than what the skill is for, so
`Skill_WarMachine_FlameThrower` and a channelled attack land in the same group
as sprinting. It is also why climbing appears in both: taking a climbing move
is charged once, hanging there drains.

## Where the numbers are

Not in a static table of anyone's first guess. All 551 record types the exe
names in its own load-failure messages were listed, and the only field matching
"stamina" is `EquipTypeInfo._isShowStamina`, a UI flag.

The costs are `UseResourceStat` entries hanging off the `skill` table's
`_useResourceStatList`, one per skill that spends stamina, each naming the
Stamina status by its row index and carrying an int64 `_varyStatAmount` that is
negative because spending is a negative change. Sprinting is -8000 a tick and
climbing -10000.

Every entry also names `Stamina_UseResourceIncreaseRate` and
`Stamina_UseResourceDecreaseRate` as the two stats that modulate it, which is
the machinery the game's own equipment socket and Abyss passive already use.

## Building

    mod\build.bat

From PowerShell, by full quoted path. It needs MSVC Build Tools 2022 and uses
the CMake and Ninja they bundle. Output is `mod\dist\StaminaMaster.asi` with
`StaminaMaster.ini` beside it.

## Installing

Copy both files into the game's `bin64` next to `CrimsonDesert.exe`, with the
game closed. An ASI loader has to be there already; on a DMM install that is
`winmm.dll`. The settings are read once at startup, so a change to the ini
needs the game restarted.

`bin64\StaminaMaster.log` says what it did, and the sessions before it are
`StaminaMaster.01.log` upwards.

Its last line counts the reads that faulted and were caught while the plugin
looked for the skill table. A handful is the normal number, usually under ten.
Candidate pointers are checked against a map of mapped memory before they are
read, and the few that still fault are ones where the memory changed between
the check and the read. A count in the hundreds of thousands means that map is
not doing its job, which is worth reporting. A crash reporter or another mod
naming `StaminaMaster.asi` in a first-chance fault line is seeing one of these,
and it is caught rather than survived.

## Licence

MIT. See LICENSE.
