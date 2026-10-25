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

## Expanded source-format roadmap (October 2026)

Prioritize by **file format**, not just engine name. Many derivatives share
formats but add incompatible commands, flags, scripts and fields.

| Tier | Source formats and known variants | Import strategy |
| --- | --- | --- |
| 1 | tbaMUD, CircleMUD, classic DikuMUD | Read indexed files; remap all VNUMs and references; boot-test |
| 1 | Merc, ROM, QuickMUD, Envy, RoT, 1stMUD and ROM-derived forks | Parse tilde-delimited .are; check extensions and mechanics |
| 1 | SMAUG, SMAUG FUSS, AFKMud, SWR/Star Wars Reality, SMAUG-derived forks | Multiple parser versions; translate resets, objects, mobiles, scripts |
| 2 | ACK!MUD, GodWars Deluxe, Devil's Silence | Dedicated .are readers and semantic mappings |
| 2 | SillyMUD (including heavily customized derivatives) | Parse monolithic tinyworld data; separately validate semantics |
| 2 | Medievia | Parse monolithic data and per-zone room files |
| 2 | CoffeeMUD | Parse .cmare XML exports, map abilities and objects |
| 3 | Dawn of Time, DikuMUD3/Valhalla, AwakeMUD, LuminariMUD, OpenRPI | Source-specific adapters after reviewing real area data |
| 4 | LPMud/FluffOS/LDMud/MudOS, Evennia, NakedMud | Extract data and reimplement behavior; not directly compatible |
| 4 | MOO/MUSH/MUX/MUCK, AberMUD and unrelated engines | Schema-specific reconstruction; scripts cannot simply be copied |

The available area_reader library currently advertises readers and writers
for ROM, Merc, GodWars Deluxe, Devil's Silence, ACK!MUD, SMAUG, SWR/FUSS,
CircleMUD, tbaMUD, Medievia and CoffeeMud. This is promising **parsing
support**, not proof of correct translation *to tbaMUD*.
Source: https://github.com/ctoth/area_reader

Possible source catalogs:
- https://www.circlemud.org/pub/CircleMUD/contrib/areas/
- https://www.grimwheel.com/codebases/
- https://godwars.net/~mudbytes/codebases.html
- https://github.com/okeuday/sillymud
- https://github.com/nickgammon/smaugfuss

The target goal is a **typed intermediate area representation** with
rooms, mobiles, objects, reset commands, scripts, shops, quests and all
cross-references; a separate adapter per original format; strict validation;
and one versioned tbaMUD writer. Do not infer licensing from source
availability. Unsupported mechanics must produce a conversion report and
prevent activation, not silently disappear. No third-party content is
implicitly authorized for redistribution.

## Required source eligibility: exclude PvP MUDs

The project owner requests **no areas sourced from PvP MUDs**. Apply this
filter to the **original game/source distribution**, not to the current
engine name. A ROM, SMAUG, Merc or CircleMUD derivative can be PvE or PvP.

Before conversion, create a provenance record documenting:

- Original game / distribution, upstream URL and exact source snapshot.
- Evidence that player-versus-player combat is absent or disabled in that
  source game's normal gameplay (source policy/docs/code, not just a label).
- Licensing evidence covering redistribution of each imported area, including
  author-specific restrictions.
- Known source-file checksums, supported format, collision and conversion
  diagnostics.
- Whether the content is stock/sample world data rather than a live game's
  proprietary areas.

**Reject** PvP-focused games, games with optional or unrestricted PvP,
unknown PvP status, ambiguous permissions, missing provenance or unknown
authorship. An area being non-combat or PvE-themed does not excuse a PvP
source under this rule. A codebase being usable for PvP does not automatically
disqualify demonstrably non-PvP stock areas: evaluate the actual source.

Never automatically download and publish all archives: this is an eligibility
and permissions review, not a blanket license. Keep rejected and unresolved
sources out of the converted world and public repository.

## Additional eligibility: combat-oriented PvE only

The source game must offer **substantive player-versus-environment combat**
against NPCs or monsters. Reject social-only, peaceful exploration, building-
only, roleplay-only and other noncombat MUDs even if they have no PvP.

The eligible intersection is **PvE combat present AND PvP absent**.
A game's combat rules, available opponents and PvP restrictions must be
verified from source documentation or code. Unknowns are held for review.
Do not confuse an area's own peaceful theme with the source game's rules;
apply this criterion to the originating game, not individual rooms.

This is an import eligibility policy only; no third-party area files are
included or activated by this change.

## Joining imported zones to the existing world

After a **complete, rights-cleared, collision-free, boot-tested area** is
available, connect its entry room to a reviewed room in the existing game.
`tools/bridge_areas.py` creates a pair of reciprocal, ordinary room exits,
without modifying the original world files:

```sh
python3 tools/bridge_areas.py \
  --existing-wld lib/world/wld/0.wld --existing-room 99 \
  --imported-wld /tmp/approved-world/wld/5000.wld --imported-room 500000 \
  --direction north --staging /tmp/bridge-review
```

The example is **illustrative**, not a verified free direction or an actual
imported area. Select the actual rooms and verify that neither exit direction
is occupied. Compare staged copies with the originals and make a reviewed
commit to install them with the associated imported zone and updated indexes.
Global VNUM/reference validation and an isolated server boot are **mandatory**.
The bridge generator does not automatically install converted content, does
not map all VNUM domains, and does not grant copyright permissions.
