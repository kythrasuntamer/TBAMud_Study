# Cross-codebase area import staging

Do **not** copy downloaded files directly into `lib/world`. Existing zone
numbers, per-record VNUMs, reset commands, special functions and triggers may
conflict, even when filenames differ. Two Crimson 2 areas are installed
(zones 47 and 58), converted by `tools/convert_crimson2.py`; see
"Installed Crimson 2 areas" below. Nothing else is installed.

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
  --offset 60000 --output /tmp/chapel-converted.wld --allow-partial
```
Destination vnums must stay within 1..65534 (IDXTYPE; 65535 is NOWHERE).

The resulting `.wld` file is **not** a complete, installable zone:
it lacks converted mobiles, objects, shops, area resets and scripts, plus
a validated `.zon` file and index. Do not copy it directly to
`lib/world`. The source sample `chapel.are` contains resets and keyed
exits, so the converter will currently **reject** it, by design.
The only third-party areas in this repository are the two Crimson 2
specimens (imports/quarantine/README.md) and their converted zones 47/58.

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

This is an import eligibility policy. The only third-party areas installed
are the owner-approved Crimson 2 conversions described below.

## Joining imported zones to the existing world

After a **complete, rights-cleared, collision-free, boot-tested area** is
available, connect its entry room to a reviewed room in the existing game.
`tools/bridge_areas.py` creates a pair of reciprocal, ordinary room exits,
without modifying the original world files:

```sh
python3 tools/bridge_areas.py \
  --existing-wld lib/world/wld/0.wld --existing-room 99 \
  --imported-wld /tmp/approved-world/wld/600.wld --imported-room 60000 \
  --direction north --staging /tmp/bridge-review
```

The example is **illustrative**, not a verified free direction or an actual
imported area. Select the actual rooms and verify that neither exit direction
is occupied. Compare staged copies with the originals and make a reviewed
commit to install them with the associated imported zone and updated indexes.
Global VNUM/reference validation and an isolated server boot are **mandatory**.
The bridge generator does not automatically install converted content, does
not map all VNUM domains, and does not grant copyright permissions.

## Whole-world VNUM preflight

Run the global definition audit before preparing a bridge or adding any
candidate world files:

```sh
python3 tools/audit_world_vnums.py lib/world /tmp/approved-world
python3 tools/validate_import_refs.py lib/world /tmp/approved-world
for t in test_area_inventory test_area_safety test_import_refs \
         test_area_import_regressions test_convert_crimson2; do
  python3 -I tests/$t.py; done        # also registered with ctest
```

The audit searches every numbered file in each VNUM family rather than
relying on filename equality. It fails on cross-file VNUM collisions.
This is a **definition** collision check: it does not yet validate every
reset reference, nested container reference, special-procedure mapping, or
all possible engine-specific metadata. Never treat a passing audit as proof
that an area will boot or behave correctly.

### Activation remains gated

Do not activate until ALL are true:
1. PvE combat present; no PvP, verified source provenance.
2. Rights to redistribute every included area and attribution preserved.
3. Complete semantics for rooms, mobiles, objects, exits, resets and shops,
   with unsupported operations rejected rather than dropped.
4. Global definition and reference collision audit and consistent VNUM
   remapping.
5. Valid zone ranges/indexes and isolated test instance successfully boots.
6. Navigation, mobile spawn, combat, object pickup and zone resets tested.
7. A reviewed bridge connects an unoccupied existing exit to the imported
   area and back, with both directions tested in-game.

The tools cover steps 3-5 and 7. The two installed Crimson 2 zones passed
steps 3-6 (see below); step 1 is the owner's Crimson 2 exception, step 2
remains open (author redistribution rights unverified), and step 7 is not
done: the zones are not linked to the stock world yet.

## Crimson 2: PvP source (see the owner-approved exception below)

The Crimson-2-MUD repository has about 214 area object files in `dev/lib/areas`, plus world, mob and zone files. Its object record format has nonstandard fields and affects; direct copying is unsafe.

Crimson MUD II supports optional player killing and player looting, so under
the PvE-only policy it would be excluded; the owner made it the single
exception below, on condition that PvP features are stripped.

Evidence: https://www.mudconnect.com/cgi-bin/search.cgi?mode=mud_listing&mud=Crimson+MUD+2
Repository: https://github.com/c2admins/Crimson-2-MUD

## Explicit Crimson 2 source exception (owner-approved)

Crimson 2 is **the sole exception** to the normal ban on PvP-source MUDs. Its areas, mobiles, objects and resets may be considered for *PvE-only* conversion. This exception does not apply to other PvP games.

Conversion must eliminate PvP-only commands, scripts, rewards, flags, arena/dueling mechanics and player-targeting combat behavior; preserve normal NPC combat and PvE loot. It must reject any unsupported or ambiguous PvP feature rather than copying it over. Extracted content must also undergo independent copyright/attribution review, VNUM remapping and reference validation, equipment/stat scaling for mortal levels 1-50, and isolated Fil-C/tbaMUD load and gameplay tests before any world activation. Merely stripping labels does not prove PvP elimination.

The source repository has separate `.wld`, `.mob`, `.obj`, `.zon` files in `dev/lib/areas` and nonstandard records; these **must not** be copied into live `lib/world` unconverted. The original source exception does not automatically clear third-party content licensing.

## Installed Crimson 2 areas (zones 47 and 58)

Converted from the quarantined specimens by `tools/convert_crimson2.py`,
which maps every field by its meaning in Crimson 2's own loader (src/db.c at
c2admins/Crimson-2-MUD 2b7207b), reports every value it drops with
`file:line vnum`, and refuses unresolved references. `tests/test_convert_crimson2.py`
regenerates both zones and requires them to match the installed files byte
for byte, so they cannot drift by hand edits.

```sh
python3 tools/convert_crimson2.py imports/quarantine/crimson2/Moria34 \
  --zone 47 --offset 600 --out /tmp/c2 --drop-external \
  --name "Mines of Moria, Levels 3-4 (Crimson 2)"
python3 tools/convert_crimson2.py imports/quarantine/crimson2/Shatterstone \
  --zone 58 --out /tmp/c2 --drop-external \
  --name "Shatterstone (Crimson 2, unfinished)"
```

| Zone | Source | Vnums | Rooms / mobs / objs / resets |
| --- | --- | --- | --- |
| 47 | Moria34 | 4700-4799 (source 4100-4125, +600: they collided with stock zone 41) | 26 / 9 / 6 / 21 |
| 58 | Shatterstone | 5800-5899 (source 5830-5841, unchanged) | 12 / 2 / 1 / 0 |

What the conversion changed (the full list prints when the tool runs):
- **PvP stripped:** room flag NOPKILL (room 4715) and any ARENA flag are
  dropped; no Crimson code, commands or scripts are imported.
- Crimson-only mechanics with no tbaMUD equivalent are dropped: act2/act3
  words, creature classes (undead, giant), poisonous, regeneration, demon
  flesh, magic resist, extra attacks and attack skills, teleport-block room
  flags, room level limits, and weapon value 0.
- Casters: the Mage (4700) and Melcrin (4708) listed Crimson spells; tbaMUD
  gives them the `magic_user` spec proc (src/spec_assign.c) instead.
- Numbering that differs is mapped: anti-thief/anti-cleric bits swap,
  SPELL_IDENTIFY 53 -> 52, Crimson attack types -> tbaMUD's, object level
  from Crimson's MINLVL flags, and keyword lists lose Crimson's "mob"/"obj"
  prefix.
- Exits into Crimson's own world (4099, 5100, 21400, 21420) are dropped.

Open owner decisions:
- **Not linked.** Neither zone has an entrance from the stock world; use
  `tools/bridge_areas.py` once an entry room is chosen. Immortals can
  `goto 4700` / `goto 5830`.
- **Zone 47 duplicates stock content.** Moria34 is Crimson's edition of the
  same DikuMUD Mines of Moria levels 3-4 that ship as zone 41 (25 of 26
  rooms share vnum and name). New in it: Melcrin, the Blade of the Moors
  Trolls and the phantom.
- **Zone 58 is an unfinished upstream template** ("Blank Zone", a "Blank
  Mob", no resets); its torch carries +100 hitroll and +145 damroll. It boots
  but loads nothing until resets are written.
- Author redistribution rights remain unverified (see the README).

