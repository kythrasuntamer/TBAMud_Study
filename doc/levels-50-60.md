# Level expansion: 50 mortal / 60 Implementor

This branch establishes **mortal levels 1-50**, **immortal levels 51-60**,
with immortal rank thresholds at 51 (Immortal), 55 (God), 58 (Greater God)
and 60 (Implementor).

The 31-50 experience requirements are interpolated from the existing
level-30 to old level-31 gap. Original levels 1-30 retain their thresholds.
This is a first-pass progression model. Review combat balance, spell levels,
hitpoints, class progression, commands, and other level-dependent content
before production deployment.

## Migrating Daeros

**Stop the MUD completely** before editing player data. Do not run a login
or save concurrently with the migration. Back up the whole `lib` directory.

From the repository root, first preview:
```sh
python3 tools/migrate_daeros_to_level60.py
```
If both player and index records show the expected original level **34**,
migrate Daeros:
```sh
python3 tools/migrate_daeros_to_level60.py --apply
```
The script refuses to act on missing records, other original levels or
existing temporary files. It writes date-stamped backups of
`lib/plrfiles/A-E/daeros.plr` and `lib/plrfiles/index` before
atomically replacing each file. These two replacements are not one
transaction; if an error occurs midway, stop and restore **both** backups.

This intentionally changes **only Daeros**, never other characters.
Any other existing immortals with levels 31-34 will become mortal by the
new thresholds until explicitly and individually migrated.

Do not run this branch against a production world without checking existing
Immortal player levels and verifying tests and Fil-C builds.
