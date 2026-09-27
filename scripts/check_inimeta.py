"""Hold the INI Master metadata up against the code that reads the ini.

Stamina Master embeds its own StaminaMaster.ini as the INIMETA resource (see
mod/src/resources.rc): the file the plugin writes out on first run and the
;@ directives INI Master reads are the same text, so there is no separate
metadata file that can drift from the shipped defaults. What can still drift
is the ;@ directives against the code: a changed clamp in Apply() or a
changed fallback in ReadSettings() that nobody copied into the ini's ;@
lines or its own default value.

The code is the authority. For every key ReadSettings() in
mod/src/core/mod.cpp reads:

  - it must appear in StaminaMaster.ini's [settings] section with a ;@ type
    directive, and every key carrying a ;@ type directive must be one
    ReadSettings() actually reads.
  - the ini's own default (the value after =) must match the fallback string
    ReadSetting() is called with.
  - the ;@ min/max must match the clamp Apply() in mod/src/game/stamina.cpp
    enforces, for the three shared-range settings and MountRegenPercent.
  - the ini's own default must fall inside that clamp.

Exits non-zero on any mismatch, so it can gate a build.

    py -3 scripts/check_inimeta.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
INI = os.path.join(ROOT, "mod", "StaminaMaster.ini")
MOD_CPP = os.path.join(ROOT, "mod", "src", "core", "mod.cpp")
STAMINA_CPP = os.path.join(ROOT, "mod", "src", "game", "stamina.cpp")

# Settings whose type the code never states directly (Probe is read as a
# float and compared against 0.0f, which is how the mod treats every 0/1
# switch), so the expected ;@ type is fixed here rather than parsed out.
BOOL_KEYS = {"Probe"}


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def parse_reader(cpp):
    """key -> fallback string, from the ReadSetting(L"Key", L"fallback") calls
    inside ReadSettings()."""
    i = cpp.index("Settings ReadSettings()")
    i = cpp.index("{", i)
    depth = 0
    j = i
    while True:
        if cpp[j] == "{":
            depth += 1
        elif cpp[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = cpp[i:j + 1]
    out = {}
    for m in re.finditer(r'ReadSetting\(L"(\w+)",\s*L"([^"]*)"\)', body):
        out[m.group(1)] = m.group(2)
    return out


def parse_shared_range(cpp):
    """The 0..100 clamp shared by UsePercent, ContinuousPercent and
    MountPercent: the settings[] table names them, and the if right after
    gives the bound."""
    m = re.search(
        r"const struct \{ int v; const char\* name; \} settings\[\] = \{(.*?)\};", cpp, re.S)
    if not m:
        raise ValueError("settings[] table not found in Apply()")
    keys = re.findall(r'"(\w+)"', m.group(1))
    m2 = re.search(r"if \(s\.v < (-?\d+) \|\| s\.v > (-?\d+)\)", cpp)
    if not m2:
        raise ValueError("shared-range clamp not found in Apply()")
    lo, hi = int(m2.group(1)), int(m2.group(2))
    return keys, lo, hi


def parse_mount_regen_range(cpp):
    m = re.search(r"scale\.mountRegen < (-?\d+) \|\| scale\.mountRegen > (-?\d+)", cpp)
    if not m:
        raise ValueError("MountRegenPercent clamp not found in Apply()")
    return int(m.group(1)), int(m.group(2))


def parse_ini(text):
    """key -> {directive, ..., 'default': ini literal} for every key in
    [settings] with a ;@ line directly above it."""
    out = {}
    lines = text.splitlines()
    pending = {}
    for line in lines:
        s = line.strip()
        if s.startswith(";@") and not s.startswith(";@mod"):
            body = s[2:].strip()
            for m in re.finditer(r'(\w+)=(".*?"|\S+)|(\w+)', body):
                if m.group(1):
                    pending[m.group(1)] = m.group(2).strip('"')
                elif m.group(3):
                    pending[m.group(3)] = True
            continue
        m = re.match(r"(\w+)=(.*)$", line)
        if m and pending:
            key, val = m.group(1), m.group(2)
            pending["default"] = val
            out[key] = pending
            pending = {}
        elif not s.startswith(";") and s:
            pending = {}
    return out


def main():
    mod_cpp = read(MOD_CPP)
    stamina_cpp = read(STAMINA_CPP)
    ini_text = read(INI)

    reader = parse_reader(mod_cpp)
    shared_keys, shared_lo, shared_hi = parse_shared_range(stamina_cpp)
    regen_lo, regen_hi = parse_mount_regen_range(stamina_cpp)
    ini_keys = parse_ini(ini_text)

    errors = []

    for k in sorted(set(reader) - set(ini_keys)):
        errors.append("%s: ReadSettings() reads it and the ini has no ;@ directive for it" % k)
    for k in sorted(set(ini_keys) - set(reader)):
        errors.append("%s: has a ;@ directive and ReadSettings() never reads it" % k)

    for k, spec in ini_keys.items():
        if k not in reader:
            continue
        fallback = reader[k]
        default = spec.get("default")
        if default != fallback:
            errors.append("%s: ini default %s, ReadSettings() falls back to %s" % (k, default, fallback))

        want_type = "bool" if k in BOOL_KEYS else "int"
        got_type = spec.get("type")
        if got_type != want_type:
            errors.append("%s: ;@ type=%s, expected %s" % (k, got_type, want_type))

        if k in shared_keys:
            lo, hi = shared_lo, shared_hi
        elif k == "MountRegenPercent":
            lo, hi = regen_lo, regen_hi
        else:
            lo = hi = None

        if lo is not None:
            got_min = spec.get("min")
            if got_min is None or int(got_min) != lo:
                errors.append("%s: ;@ min=%s, Apply() clamps to %s" % (k, got_min, lo))
            got_max = spec.get("max")
            if got_max is None or int(got_max) != hi:
                errors.append("%s: ;@ max=%s, Apply() clamps to %s" % (k, got_max, hi))
            try:
                if not (lo <= int(default) <= hi):
                    errors.append("%s: ini default %s falls outside the %s..%s Apply() clamps to"
                                  % (k, default, lo, hi))
            except (TypeError, ValueError):
                errors.append("%s: ini default %r is not an int" % (k, default))

    for line in errors:
        print("error  " + line)
    print("%d keys read by ReadSettings(), %d described in the ini, %d errors"
          % (len(reader), len(ini_keys), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
