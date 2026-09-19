# Unlimited Stamina

A stamina plugin for Crimson Desert 2.03.00 (exe 1.0.0.2944).

**This is not a release.** The only thing in here that runs is a probe: it reads
the game's data tables at startup, writes what it found to a log, and changes
nothing. There is no download and no mod page yet.

## What it is for

Nexus mod 107, Infinite Stamina by PhorgeForge, reduces the stamina cost of
sprinting, gliding, climbing, swimming and riding. It was last updated in April
2026 and it works by replacing game data: an archive that drops a `0036` folder
into the game root and overwrites `meta\0.papgt`. Two game patches have landed
since, and on a current install its `0.papgt` is also the wrong one to use, so
following its instructions removes the two pack groups the DMM mod manager adds.

This does the same job as a plugin instead. No game file is touched, and how
much stamina you save is a number in an ini rather than a choice between five
downloads.

## Where the stamina numbers actually are

The `skill` table, 2,069 rows, through `SkillInfo._useResourceStatList`, a list
of `UseResourceStat` records whose `_varyStatAmount` is the cost. 301 rows carry
the Stamina status key, and they are the expected ones: `Skill_BasicMoveLv4` for
sprinting, `Skill_PointClimbLv0` through `Lv2`, `Skill_Swimming_Run`,
`Skill_CrowWing` and `Skill_Damian_ShieldGlider`, `Skill_HorseStamina`.

The game also carries a stat of its own called
`Stamina_UseResourceDecreaseRate`, `statusinfo` row 1000037, already granted by
an equipment socket, an Abyss passive and the horse. Whether writing it is
enough on its own is what the probe is for.

## Building

    mod\build.bat

From PowerShell, by full quoted path. It needs MSVC Build Tools 2022 and uses
the CMake and Ninja they bundle. Output is `mod\dist\UnlimitedStamina.asi` and
`UnlimitedStamina.ini` beside it.

## Installing the probe

Copy both files into the game's `bin64` next to `CrimsonDesert.exe`, with the
game closed. An ASI loader has to be present already; on this install that is
`winmm.dll`. Run the game, play for a minute with a mount, a climb and a swim,
then read `bin64\UnlimitedStamina.log`.

## Licence

MIT. See LICENSE.
