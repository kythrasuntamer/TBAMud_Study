# Cross-codebase area import staging

Do **not** copy downloaded files directly into `lib/world`. Existing zone
numbers, per-record VNUMs, reset commands, special functions and triggers may
conflict, even when filenames differ. No area has been imported by this PR.

## Sources to review

- CircleMUD contributed areas: https://www.circlemud.org/pub/CircleMUD/contrib/areas/
- ROM/Merc/SMAUG stock areas: review original distributions, including each
  individual area author's license or distribution terms
- Candidate parser/writer: https://github.com/ctoth/area_reader

Being publicly downloadable does not itself grant redistribution rights.
Keep the original README, copyright information, provenance URL and a
documented permission basis for every imported area. Avoid shipping material
without clear redistribution permission.

## Workflow

1. Extract in a separate temporary directory, rejecting symlinks, traversal
   paths, oversized entries and archive bombs. Never extract over the game.
2. Parse the source format using a verified tool (for example `area_reader`),
   translate unsupported flags and commands, and export a canonical Circle/tba
   `lib/world` tree. Inspect lost or unsupported fields.
3. Audit filename collisions and create an isolated staging tree:

   ```sh
   python3 tools/stage_areas.py /tmp/canonical-world lib/world /tmp/staged-world
   python3 tools/stage_areas.py /tmp/canonical-world lib/world /tmp/staged-world --write
   ```

4. Check **all** record VNUMs, including rooms, mobiles, objects, shops,
   triggers and quests. Remap both definitions and references consistently
   before proposing integration. Resolve external exits and zone reset targets.
5. Use an isolated server with a copy of the world database to boot-test
   the staged area, scripts and resets. Check logs, navigation, combat and shops.
6. Only then add reviewed area files and indexes in a separate PR.

The staging helper intentionally does **not** automate dangerous VNUM remaps,
execute source scripts, or modify the installed world. It rejects direct file
collisions but **does not prove** that non-overlapping filenames have
non-overlapping VNUMs.

## SillyMUD and Ethereal II recovery track

Stock SillyMUD 1.1b files are preserved in:
https://github.com/okeuday/sillymud/tree/master/silly/lib

This tree contains `tinyworld.mob`, `tinyworld.obj`, `tinyworld.shp`,
`tinyworld.wld`, and `tinyworld.zon`, plus a `world` directory.
Unlike tbaMUD's numbered per-zone files, the stock SillyMUD files include
combined world tables; zone records have a different numeric header layout
(e.g. `#2 / LIMBO~ / 49 1 512`) and need an explicit SillyMUD parser,
schema mapping, and record-based VNUM collision analysis. **Do not simply
rename these files to tbaMUD filenames** or treat them as interchangeable.

SillyMUD's original source repository identifies the DikuMUD license; audit
original area creator rights and distribution conditions before shipping
converted area content in a public repository. Source availability does not
establish permission to republish individual zones.

Ethereal II reportedly derives from a heavily customized SillyMUD. Stock
SillyMUD world files are **not** an authenticated Ethereal II backup; keep
Ethereal II recovery separate and require original files and permission.

Next implementation milestones:
1. Inventory records and directives in the SillyMUD monolithic files.
2. Convert rooms/exits, mobiles, objects, shops and zone resets with
   explicit flags/special-procedure translation or a hard failure.
3. Allocate isolated destination VNUM ranges, rewrite every cross-reference,
   and reject unresolved external links and unsupported script calls.
4. Run isolated tbaMUD boot, navigation, shop and reset tests.

## Experimental SMAUG FUSS room converter

`tools/convert_smaug_rooms.py` parses `#ROOM` / `#EXIT` blocks from FUSS
`.are` files and converts basic geometry into a **room-only** tbaMUD
`.wld` file. It can relocate room VNUMs by an explicit offset, and it
rejects unresolved exits, keyed doors, resets, unsupported room/exit fields,
invalid VNUMs and duplicate directions rather than silently losing critical
gameplay. Other original areas may not be supported.

Example using a *locally obtained and rights-cleared* file:
```sh
python3 tools/convert_smaug_rooms.py /tmp/chapel.are \
  --offset 500000 --output /tmp/chapel-converted.wld --allow-partial
```

The resulting `.wld` file is **not** a complete, installable zone:
it lacks converted mobiles, objects, shops, area resets and scripts, plus
a validated `.zon` file and index. Do not copy it directly to
`lib/world`. The source sample `chapel.are` contains resets and keyed
exits, so the converter will currently **reject** it, by design.
No third-party areas have been committed to this repository.

## SillyMUD converter status

Stock SillyMUD combines data into `tinyworld.*` rather than the tbaMUD
per-zone layout. Its zone headers and reset semantics are different. A
dedicated conversion implementation and tests are still needed. **Do not**
pass SillyMUD files to the SMAUG converter; do not claim Ethereal II areas
are present in the stock SillyMUD distribution.
