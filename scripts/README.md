# scripts

## scan_packs.py

Walks every pack group under the game root that holds a `0.pamt`, parses the
index, and prints the entries matching a term. Use it to find which group owns
a file before extracting it.

    py -3 scan_packs.py stamina
    py -3 scan_packs.py --ext .paac --group 0010
    py -3 scan_packs.py --ext .staticinfobody --group 0008 --count-only

It imports `paz_parse` from the unpacker under `master looter\tools`, so that
checkout has to be present. `--game` points it at a different install. The exit
code is 1 when nothing matched.

What the groups hold, for the ones that have come up so far:

    0008    static tables (*.staticinfobody / *.staticinfoheader) and the
            per-prefab *.binarygimmick records
    0010    actionchart\*.paac, the action charts, and gamedata\*.xml
    0020    string tables (*.paloc)

Extracting is the unpacker's own `paz_unpack.py`:

    py -3 paz_unpack.py "<game>\0010\0.pamt" --paz-dir "<game>\0010" -o out --filter "*.paac"
