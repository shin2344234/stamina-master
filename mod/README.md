# Stamina Master 1.0.0

For Crimson Desert 2.03.00, exe 1.0.0.2944.

Out of the box nothing you do on foot costs stamina, and a horse recovers at a
gallop about as fast as it does standing still. Every number is a setting, so
if you would rather have it cheaper than free, change one line.

## Installing

Copy `StaminaMaster.asi` and `StaminaMaster.ini` into the game's `bin64`
folder, next to `CrimsonDesert.exe`, with the game closed. You need an ASI
loader there already; Ultimate ASI Loader installed as `winmm.dll` is what most
Crimson Desert setups use, and the Definitive Mod Manager installs one for you.

To uninstall, delete both files. No game file is modified and there is no meta
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

## Licence

MIT. See LICENSE. Third-party components are listed in
THIRD_PARTY_NOTICES.md.
