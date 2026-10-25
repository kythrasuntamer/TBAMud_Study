# Cross-codebase class and skill-tree conversion

The 50/60 level-cap branch is the baseline. Class 0 is presented as Wizard; its persisted numeric ID and internal CLASS_MAGIC_USER constant remain unchanged. The legacy M selector remains valid, with Z offered for Wizard alongside W for Warrior. **No additional classes are yet
playable.** The four original classes now have proposed six-step skill trees in the manifest, but these are NOT implemented abilities. Never replace the class IDs 0-3 used by saved characters.

Verified class roster in
https://github.com/moshehbenavraham/LuminariMUD (`src/class.c`,
`src/structs.h`): Wizard, Cleric, Rogue, Warrior, Monk, Druid, Berserker,
Sorcerer, Paladin, Ranger and Bard. The first four correspond to existing
tbaMUD classes. Seven additional classes have been inventoried with proposed
stable IDs 4-10; those IDs are not installed in C code.

Other potential class/skill systems:
- https://github.com/Arthmoor/AFKMud (`system/skills.dat`, `src/raceclass.h`)
- https://github.com/EmpireMUD/EmpireMUD-2.0-Beta (`src/class.c`, `src/skills.c`)
- https://github.com/MUDOmnibus/QuickMUD (`src/skills.c`, `doc/class.txt`)

These engines do not share identical skill-tree semantics. LuminariMUD
has class progression and spell access; EmpireMUD uses classes with skill
requirements and abilities. A raw name or data copy is not a working class.

Run:
```sh
python3 tools/validate_class_import.py data/class_import_candidates.json
python3 -m unittest discover -s tests -p 'test_class_import.py'
```

### Gates before making classes selectable

1. Verify each source's license and original author permissions.
2. Map every imported spell, feat, talent and skill to a supported effect
   or implement it with gameplay tests (no fake placeholder skills).
3. Assign unique class IDs, spell/skill IDs, and sensible prerequisite trees
   for character levels 1-50, with all saves, THAC0, hitpoints, mana,
   training limits and abilities balanced to comparable existing classes.
4. Update character creation, guildmasters, combat, spells, equipment
   restrictions, display and persistence without changing saved IDs 0-3.
5. Run native, sanitizers, Fil-C and in-game leveling, training, combat
   and save/reload tests before merging. Keep class work out of the area
   importer so each integration can be reviewed independently.

The current manifest is a **source inventory**, not a complete skill tree;
none of the source mechanics have been transplanted yet.
