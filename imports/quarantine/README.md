# External MUD area acquisition ledger

All files in this folder are **unconverted source specimens**. They are not
indexed by tbaMUD and must never be copied into `lib/world` directly; the
installed zones 47 and 58 are generated from them by
`tools/convert_crimson2.py` (doc/area-imports.md).

## Crimson 2 source specimens

- Upstream: https://github.com/c2admins/Crimson-2-MUD
- Original paths: `dev/lib/areas/Moria34.{wld,mob,obj,zon}` and
  `dev/lib/areas/Shatterstone.{wld,mob,obj,zon}`
- Acquisition: 2026-10-25, via upstream GitHub repository
- Import authorization: user approved an exception for Crimson 2 only
- PvP: the specimens contain no code or scripts; the only PvP data, room
  flag NOPKILL in Moria34 room 4115, is dropped by the converter
- Per-area author redistribution rights: **not verified** (owner accepted
  including the files)
- VNUM collisions: **yes**, Moria34 4100-4125 collides with stock zone 41;
  installed remapped to 4700-4799 (zone 47). Shatterstone 5830-5841 is
  free (zone 58)
- Compatibility with tbaMUD: converted and boot-verified (`circle -c`,
  ASan+UBSan), see doc/area-imports.md
- Installed: zones 47 and 58, **not linked** to the stock world yet

The source repository has an LGPL-2.1 license file and README referring to
DikuMUD licensing, but this alone does not demonstrate permission from each
original area author. Preserve attribution and seek clearance before
redistributing these specimens beyond this development workspace.

## Other source repositories with publicly visible data (not yet copied)

| Repository | Available area data found |
| --- | --- |
| https://github.com/moshehbenavraham/LuminariMUD | 236 .wld, 236 .mob, 236 .shp, 237 .zon |
| https://github.com/Arthmoor/AFKMud | 9 .are |
| https://github.com/MUDOmnibus/QuickMUD | 53 .are |
| https://github.com/gblues/ROM | 53 .are |
| https://github.com/ozgyilmaz/Anatolia-Mud | 109 .are |
| https://github.com/nickgammon/smaugfuss | 25 .are |

These are repository-wide suffix counts, not unique, playable, legally cleared
zones. They may contain examples, duplicates, experimental files, or PvP mechanics.
