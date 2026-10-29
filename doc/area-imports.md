# Cross-codebase area import staging

Do **not** copy downloaded files directly into `lib/world`. Existing zone
numbers, per-record VNUMs, reset commands, special functions and triggers may
conflict, even when filenames differ. Two Crimson 2 areas are installed by
`tools/convert_crimson2.py` (Moria34 merged into stock zone 41, Shatterstone
as zone 58); see "Installed Crimson 2 areas" below. Nothing else is installed.

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
specimens (imports/quarantine/README.md) and what was installed from them.

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

The tools cover steps 3-5 and 7. The installed Crimson 2 content passed
steps 3-7 (see below); step 1 is the owner's Crimson 2 exception and step 2
remains open (author redistribution rights unverified).

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

## Installed Crimson 2 areas

`tools/convert_crimson2.py` converts by the meaning each field has in
Crimson 2's own loader (src/db.c at c2admins/Crimson-2-MUD 2b7207b), reports
every value it drops with `file:line vnum`, and refuses unresolved
references. What is installed, and how, is recorded in
`imports/quarantine/crimson2/recipe.json`; reproduce it with

```sh
python3 tools/convert_crimson2.py --recipe imports/quarantine/crimson2/recipe.json \
  --area Shatterstone --out /tmp/c2 --install lib/world
python3 tools/convert_crimson2.py --recipe imports/quarantine/crimson2/recipe.json \
  --area Moria34 --out /tmp/c2 --install lib/world
```

(`--install` refuses to overwrite a zone or to append a merge twice; it
checks every target first and then moves each file into place from a
temporary sibling, so a refused install changes nothing).
`tests/test_convert_crimson2.py` regenerates both from the recipe and requires
zone 58 to match byte for byte and zone 41 to end with exactly the merged
Moria34 additions, so neither can drift by hand edits.

### Moria34: merged into stock zone 41

Moria34 is Crimson's edition of the DikuMUD Mines of Moria levels 3-4 that
already ship as zone 41: all 26 of its rooms are zone 41 rooms (25 with the
same name; 4115 differs only in case). So no rooms are added. Only what is
new is merged, into zone 41's own rooms, as Crimson's zone file placed it:

| New in zone 41 | From Moria34 | Loaded |
| --- | --- | --- |
| mob 4108 Melcrin, sovereign of the trolls | mob 4108 | room 4115 (max 1), wielding obj 4105 |
| mob 4109 the phantom | mob 4104 (stock 4104 is the orc) | rooms 4106 and 4113 (max 4) |
| obj 4105 Blade of the moors trolls | obj 4105 | worn by Melcrin |

**Rescaled to the zone** (owner decision, 2026-10-28). Zone 41's stock mobs
are levels 6-13, so Crimson's numbers would have put a near-max-level caster
in a low-level dungeon. The recipe overrides them (each override is printed
when the tool runs), modelled on stock tbaMUD mobs of the target level:
level-16 references such as 1.mob #169 and 120.mob #12028 (3d3+160, 2d2+2,
THAC0 15, AC 0, 25600 exp), level-11 references such as 40.mob #4000 and
4.mob #421 (2d2+110, THAC0 17, AC 3, 12100 exp). AC is in file units (the
game multiplies by 10).

| | Level | THAC0 | AC | HP | Damage | Gold | Exp | Flags |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Melcrin before | 48 | 4 | -35 | 35d10+24000 | 20d20+50 | 50000 | 1500000 | aggressive to all, sentinel; sanctuary |
| Melcrin after | 16 | 15 | -1 | 4d4+200 | 2d4+3 | 400 | 30000 | sentinel; sense life |
| phantom before | 18 | 22 | 26 | 10d29+150 | 4d5+15 | 0 | 5480 | aggressive to all, scavenger, stay-zone, wimpy |
| phantom after | 11 | 17 | 3 | 2d2+110 | 1d4+1 | 0 | 12100 | scavenger, stay-zone, wimpy |

The Blade of the moors trolls drops from a level-16 boss, so it is priced like
stock level 10-20 weapons (2d9 to 6d3, cost 300-1500): 11d5 slash, +4
damroll, level 41, cost 40000 / rent 3000 became 3d5 slash, +2 damroll,
level 15, cost 1500 / rent 150 (weight 10 and its anti-good flag kept).

Melcrin cast spells in Crimson, so it gets tbaMUD's `magic_user` spec proc
(src/spec_assign.c). The Mage needs nothing: its counterpart, stock 4100,
already casts through DG trigger 4100. Crimson's other zone 41 variants (the
mage, troll, snake, thief, centipede, warrior, kobold and their items) are
the stock records; zone 41 keeps its own.

### Shatterstone: zone 58

| Zone | Vnums | Rooms / mobs / objs / resets |
| --- | --- | --- |
| 58 Shatterstone (Crimson 2, unfinished) | 5830-5841 | 12 / 1 / 1 / 4 |

- **Linked** from the Dwarven Kingdom: room 6542 "At The Bottom Of The
  Mineshaft" `down` <-> 5830 "Overlooking the cavern" `up`, a plain two-way
  exit (no door, no key). A deeper descent from the dwarven mines opens on a
  ledge over a ruined dwarven city; zone 65 is the stock dwarven zone (levels
  3-33) and 6542 is the deepest point of its mine.
- **Resets come from the recipe.** The upstream zone is an unfinished
  template ("Blank Zone", a "Blank Mob", no resets), so the guard and the
  torch never loaded. The rooms say where they belong: 5832 sees "a massive
  gate guarded by two dwarven guards", and in 5833 "Before the gates" the
  guards turn out to be spirits. The recipe's `"resets"` (source vnums,
  checked like the source's own) load two guard spirits 5830 in 5833, each
  holding the torch 5830 in its light slot (`E ... 0`, WEAR_LIGHT): the torch
  is a take+hold light with no other wear flag. The zone header becomes
  lifespan 20, reset mode 2 (`"lifespan"`, `"reset_mode"`), like zone 65; the
  source's `1 0` would never reload a killed guard.
- **Exit text.** 5830's `down` exit had garbled source text (description
  "steps going down.Steps going down", keywords "steps going down."). The
  recipe's `"exits"` replaces it with "A broad stairway leads down into the
  city below." and no keywords (no door), matching 5831's stairway.
- **The guard is a real mob; the template mob is gone.** Mob 5830 was an
  unedited template (keywords "blank", level 48, 1d1+1 hp, 99d99-25 damage,
  AC -40, 2000000 exp, 100000 gold, aggressive to all). The recipe makes it a
  non-aggressive level-20 sentinel like stock level-20 guards (269.mob #26909:
  4d4+200, 3d3+3, THAC0 14, AC -2), with keywords "guard spirit dwarven dwarf"
  and texts taken from the source description ("the spirit of a dwarven
  guard"). Mob 5831 "the Blank Mob" had no content to be faithful to and is
  excluded (`"exclude"` in the recipe).
- **The torch is an ordinary light.** In the template it carried +100 hitroll
  and +145 damroll, and its keywords ("rusty hoop") did not include "torch".
  The recipe overrides it to match the stock torch (obj 3030): take+hold, 24
  hours of light, weight 1, cost 10, rent 10, level 0, keywords
  "torch rusty hoop". Each guard holds one.

### What the conversion changed (the full list prints when the tool runs)
- **PvP stripped:** room flag NOPKILL (Moria34 room 4115, not installed) and
  any ARENA flag are dropped; no Crimson code, commands or scripts are imported.
- Crimson-only mechanics with no tbaMUD equivalent are dropped: act2/act3
  words, creature classes (undead, giant), demon flesh, magic resist, extra
  attacks and attack skills, room level limits and weapon value 0.
- Numbering that differs is mapped: anti-thief/anti-cleric bits swap,
  SPELL_IDENTIFY 53 -> 52, Crimson attack types -> tbaMUD's, object level
  from Crimson's MINLVL flags, colour codes `&X` -> `@X`, and keyword lists
  lose Crimson's "mob"/"obj" prefix.

Open: author redistribution rights remain unverified (see the README).
