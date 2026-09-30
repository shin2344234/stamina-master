# Stamina Master 1.0.3

For Crimson Desert 2.03.02, exe 1.0.0.2976. It reads the skill table by its
layout and not by address, so 2.03.00 runs it too.

Out of the box nothing you do on foot costs stamina, and a horse recovers at a
gallop about as fast as it does standing still. Every number is a setting, so
if you would rather have it cheaper than free, change one line.

## Installing

Copy `StaminaMaster.asi` into the game's `bin64` folder, next to
`CrimsonDesert.exe`, with the game closed. The first time it runs it writes
`StaminaMaster.ini` beside itself with every setting at its default, and an
ini you already have is left alone. You need an ASI
loader there already; Ultimate ASI Loader installed as `winmm.dll` is what most
Crimson Desert setups use, and the Definitive Mod Manager installs one for you.

To uninstall, delete the `StaminaMaster` files from `bin64`. No game file is modified and there is no meta
patch to undo.

The settings are read once when the game starts, so a change to the ini needs
a restart.

## Settings

All of them live in `StaminaMaster.ini`.

    UsePercent=0            costs charged once per use, 130 of them
    ContinuousPercent=0     drains that run while you hold them, 61
    MountPercent=0          things you do while mounted, 15
    MountRegenPercent=1000  how fast a horse recovers, 100 to 10000
    Probe=0                 write the research report to the log

The first three are a percentage of the game's own cost. 100 leaves that kind
alone, 0 removes it. A value outside 0 to 100 refuses the whole write, so a
typo changes nothing rather than half of it.

INI Master (https://www.nexusmods.com/crimsondesert/mods/3578) can edit these
with a label, a range and the default for each, all read out of the plugin
itself. The plugin reads its settings once at startup, so a change takes
effect on the next launch.

`MountRegenPercent` works the other way round, because a horse does. Its gait
is a recovery rate that falls as it speeds up, and it tires at speed because
something drains faster than that rate. Raising the recovery is what stops it.
100 leaves the game's own rates alone; the default of 1000 puts a full gallop
at roughly what standing still gives.

Skills that give **you** stamina back are never touched, so food, rest and
recovery abilities restore exactly what they always did.

## What it reaches

Sprinting, climbing, swimming, the glider, the rocket pack, rolls, jumps, every
weapon swing, grapples, bow shots, the war machine's weapons, and riding.

Combat is included because the game keeps no flag anywhere that separates
travelling from fighting. Set `UsePercent` and `ContinuousPercent` to 100 if you
want the mod on your horse and nowhere else.

## If something looks wrong

`StaminaMaster.log` sits beside the plugin and says what it changed. Set
`Probe=1` and restart for a line per skill, plus a re-read a minute and two
minutes in that confirms the changes are still there. That log is the useful
thing to attach to a bug report.

The last line of that log counts the reads that faulted and were caught while
the plugin looked for the skill table. A few dozen at most is normal.
Candidate pointers are checked against a map of mapped memory first, and the
few that still fault are ones where the memory changed in between. A count in
the hundreds of thousands is worth reporting. If a crash reporter or another
mod names StaminaMaster in a first-chance fault line, that is what it is
seeing, and it was caught rather than survived.

## Licence

MIT. See LICENSE. Third-party components are listed in
THIRD_PARTY_NOTICES.md.
