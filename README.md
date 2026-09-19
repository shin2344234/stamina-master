# Unlimited Stamina

A stamina plugin for Crimson Desert 2.03.00 (exe 1.0.0.2944).

It multiplies the stamina cost of what you do, so sprinting, climbing,
swimming, gliding and riding cost a fraction of what the game charges. How much
of a fraction is three numbers in an ini rather than a choice between five
downloads, and no game file is touched.

Not released yet. There is no mod page and no download.

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

    UsePercent=25          130 costs charged once per use: a roll, a jump, a swing
    ContinuousPercent=10    61 drains that run while held: sprint, climb, swim, glide
    MountPercent=25         15 things you do while mounted
    Probe=0                 write the research report and a line per skill changed

Each is a percentage of the game's own cost, so 100 leaves that kind alone and
0 removes its cost outright. A value outside 0 to 100 refuses the whole write,
including whichever settings were in range.

Skills that give stamina back, 50 of the 256 entries, are never touched. Food
and rest restore what they always did.

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
the CMake and Ninja they bundle. Output is `mod\dist\UnlimitedStamina.asi` with
`UnlimitedStamina.ini` beside it.

## Installing

Copy both files into the game's `bin64` next to `CrimsonDesert.exe`, with the
game closed. An ASI loader has to be there already; on a DMM install that is
`winmm.dll`. The settings are read once at startup, so a change to the ini
needs the game restarted.

`bin64\UnlimitedStamina.log` says what it did, and the sessions before it are
`UnlimitedStamina.01.log` upwards.

## Licence

MIT. See LICENSE.
