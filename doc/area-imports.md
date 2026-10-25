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
