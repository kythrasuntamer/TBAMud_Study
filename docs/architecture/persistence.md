# Persistence: what is on disk and how it is read and written

This document describes every file the server reads or writes under `lib/`:
the world files and how `db.c` loads them, the player index and player
files (`players.c`), rented objects (`objsave.c`), houses (`house.c`), mail
(`mail.c`), bulletin boards (`boards.c`), ideas/bugs/typos (`ibt.c`) and the
game configuration (`config.c`, `cedit.c`, `db.c:load_config`). It closes
with how each loader and saver behaves when something goes wrong.

The OLC savers that write world files (`genwld.c`, `genmob.c`, `genobj.c`,
`genzon.c`, `genshp.c`, `genqst.c`, `dg_olc.c`) are covered in
[olc.md](olc.md). Trigger files are covered in
[dg-scripts.md](dg-scripts.md). The boot order is in
[overview.md](overview.md#boot-sequence).

## Contents

1. [The lib tree](#the-lib-tree)
2. [World loading](#world-loading)
3. [World file formats](#world-file-formats)
4. [Text files and help](#text-files-and-help)
5. [Player index and player files](#player-index-and-player-files)
6. [Rent and crash files](#rent-and-crash-files)
7. [Houses](#houses)
8. [Mail](#mail)
9. [Bulletin boards](#bulletin-boards)
10. [Ideas, bugs and typos](#ideas-bugs-and-typos)
11. [Configuration](#configuration)
12. [Failure behaviour](#failure-behaviour)

## The lib tree

The server `chdir()`s into `lib/` at startup, so every path below is
relative to it. Paths come from `db.h` (`LIB_*`, `*_FILE`, `*_PREFIX`).

| Path | Format | Written by | Read by |
| --- | --- | --- | --- |
| `world/{wld,mob,obj,zon,shp,trg,qst}/index`, `index.mini` | list of file names, `$` at the end | `genzon.c:create_world_index` | `db.c:index_boot` |
| `world/wld/N.wld`, `mob/N.mob`, `obj/N.obj`, `zon/N.zon` | `#`-records, `~`-terminated strings | OLC savers | `db.c` parsers |
| `world/shp/N.shp`, `qst/N.qst`, `trg/N.trg` | as above | OLC savers | `shop.c`, `quest.c`, `dg_db_scripts.c` |
| `text/{news,motd,imotd,credits,greetings,...}` | plain text with `@` colour codes | `tedit` (and administrators) | `db.c:file_to_string_alloc` |
| `text/help/index`, `*.hlp`, `help`, `ihelp` | keyword blocks | `hedit` | `db.c:load_help` |
| `misc/messages`, `misc/socials`, `socials.new` | combat messages, socials | `msgedit`, `aedit` | `msgedit.c:load_messages`, `db.c:boot_social_messages` |
| `misc/xnames` | one forbidden substring per line | administrators | `ban.c:read_invalid_list` |
| `misc/{ideas,bugs,typos}` | keyword records | `ibt.c:save_ibt_file` | `ibt.c:load_ibt_file` |
| `etc/config` | `tag = value` lines | `cedit.c:save_config` | `db.c:load_config` |
| `etc/badsites` | `type site date name` lines | `ban.c:write_ban_list` | `ban.c:load_banned` |
| `etc/plrmail` | `### to from time` + body | `mail.c` | `mail.c` |
| `etc/board.*` | binary records | `boards.c:board_save_board` | `boards.c:board_load_board` |
| `etc/hcontrol` | binary `struct house_control_rec` array | `house.c:House_save_control` | `house.c:House_boot` |
| `etc/time` | one number: mud time in seconds | `db.c:save_mud_time` | `db.c:reset_time` |
| `etc/last` | binary `struct last_entry` records | `act.wizard.c:add_llog_entry` | `last` command, `clean_llog_entries` |
| `plrfiles/index` | `id name level flags last` lines, `~` at the end | `players.c:save_player_index` | `players.c:build_player_index` |
| `plrfiles/X-Y/name.plr` | tagged text | `players.c:save_char` | `players.c:load_char` |
| `plrobjs/X-Y/name.objs` | rent header + object records | `objsave.c` | `objsave.c` |
| `plrvars/X-Y/name.mem` | DG global variables (legacy) | DG code | `dg_scripts.c:read_saved_vars` |
| `house/N.house` | object records | `house.c:House_crashsave` | `house.c:House_load` |
| `copyover.dat` | copyover state, deleted once read | `act.wizard.c:do_copyover` | `comm.c:copyover_recover` |

Player files are spread over subdirectories by the first letter of the
lower-cased name: `A-E`, `F-J`, `K-O`, `P-T`, `U-Z`, and `ZZZ` for anything
else (`utils.c:get_filename`).

## World loading

### Index files and the two passes

`db.c:index_boot(mode)` loads one kind of world file. It opens
`world/<kind>/index` (or `index.mini` in mini-MUD mode), which lists data
file names one per line and ends with a line starting with `$`.

1. **Counting pass.** Each listed file is opened and its records counted
   (`count_hash_records()` counts lines starting with `#`; help files use
   `count_alias_records()`; zone files count as one record each). A file
   that cannot be opened is reported and skipped in this pass.
2. **Allocation.** The table is allocated for that many records:
   `world[]`, `mob_proto[]`/`mob_index[]`, `obj_proto[]`/`obj_index[]`,
   `zone_table[]`, `help_table[]`, `aquest_table[]` or `trig_index[]`. Zero
   records is fatal except for shops and quests.
3. **Loading pass.** Each file is opened again (failure is now fatal) and
   handed to `discrete_load()` (rooms, mobiles, objects, triggers, quests),
   `load_zones()`, `load_help()` or `shop.c:boot_the_shops()`.

`db.c:discrete_load` reads `#<number>` record headers and calls the parser
for that record type with the number. A `$` line ends the file; a record
number of 99999 or more also ends it. Object files have no end-of-record
marker, so `parse_object()` returns the next header line it read.

Each parser fills the next free slot of its table and updates the top
counter (`top_of_world = room_nr++` and similar). The slot counter is a
static variable inside the parser, so each parser must be called exactly
once per record, in file order, during boot. On the base branch nothing
checks the slot against the allocation; a `#` line that the counting pass
missed but the parser read writes past the table. Branch
`audit-deferred-db-loaders` (commit `432130f`) checks it.

### String readers

World strings end with a `~`. `db.c:fread_string`:

- reads lines with `fgets()` until one ends in `~` (after line endings are
  stripped), converting every line ending to `\r\n`;
- converts `@` colour codes to `\t` (`parse_at()`);
- returns a `strdup()`ed copy, or **`NULL` for an empty string**; callers
  must accept `NULL`;
- calls `exit(1)` at end of file before the `~`, or if the string exceeds
  `MAX_STRING_LENGTH` (48 KB).

`fread_clean_string()` does the same but skips leading spaces. The token
readers `fread_number()`, `fread_line()`, `fread_flags()`, `fread_word()`
and `fread_to_eol()` serve the keyword formats (IBT, quests).

`utils.c:get_line` reads the next line that is not blank and does not start
with `*` (comment lines), strips the line ending, and copies it into a
caller buffer of at least `READ_SIZE` (256) bytes. A longer line is cut;
the rest of the line is discarded and reported, so it never comes back as a
separate line. It returns 0 at end of file without touching the buffer.

### Parse context

A loader can register a `FILE *` with `tb_parse_context_begin(stream,
resource, parser)` and update the current record and field with
`tb_parse_context_record()` and `tb_parse_context_field()`. `get_line()`
counts lines for registered streams. `TB_PARSE_CONTEXT_ERROR(stream, ...)`
then logs a `PARSER` diagnostic that names the file, physical line, parser,
record and field. See [foundations.md](foundations.md#structured-diagnostics).

### Fix-ups after loading

- `renum_world()` turns every exit's destination vnum into an rnum.
- `check_start_rooms()` resolves the mortal, immortal and frozen start
  rooms. A missing mortal start room is fatal; the other two fall back to
  it.
- `renum_zone_table()` turns zone command vnums into rnums and disables
  (`'*'`) any command that names something that does not exist, with a
  `mudlog` naming the zone and line.
- `parse_object()` calls `check_object()`, which logs bad values (unknown
  flags, spell numbers, levels, container and drink values) and repairs
  some of them.

### Legacy 32-bit flag files

Old CircleMUD world files store one flag word per field; tbaMUD stores four
(128 bits). With `bitwarning` set in `config.c`, loading an old-format
record stops the boot with a warning. With it cleared, the record is
converted, and with `bitsavetodisk` set the converted zones are written back
(`converting` triggers `save_all()` at the end of `boot_world()`).

## World file formats

These are summaries; `doc/building.txt` is the full builder reference.
Flags are written as letters (`a` = bit 0 ... `z`, `A` = bit 26 ...) or as
a decimal number; `db.c:asciiflag_conv` reads both.

### Rooms (`.wld`)

```
#<vnum>
<name>~
<description>~
<zone> <flags0> <flags1> <flags2> <flags3> <sector>
D<dir>                       zero or more exits
<exit description>~
<keywords>~
<door type> <key vnum> <destination vnum>
E                            zero or more extra descriptions
<keywords>~
<text>~
S                            end of room
T <trigger vnum>             zero or more, after S
```

`parse_room()` assigns the room to the zone whose `bot..top` range contains
its vnum; a room outside every zone is fatal. Door type 1 is a door, 2 a
pick-proof door, 3 and 4 hidden variants. A key of -1 or 65535 means none; a
destination of -1 or 0 means no exit. On the base branch the sector check
lets 10 (one past the last sector) and negative values through, and the
direction after `D` is not range-checked before it indexes `dir_option[]`;
branch `audit-deferred-db-loaders` fixes both (commits `b7d184b`, `148143a`),
as does `worktree-wf_c613c2c5-f9f-41` for the direction (commit `79ffa21`).

### Mobiles (`.mob`)

```
#<vnum>
<keywords>~
<short description>~
<long description>~
<description>~
<act flags x4> <affect flags x4> <alignment> <S|E>
<level> <thac0> <ac> <hp dice XdY+Z> <damage dice XdY+Z>
<gold> <experience>
<position> <default position> <sex>
[E-specs: "Keyword: value" lines, then "E"]   enhanced (E) mobiles only
T <trigger vnum> ...
```

`parse_mobile()` sets `MOB_ISNPC`, points `player_specials` at `dummy_mob`
and stores the hit dice in `points.hit/mana/move` for `read_mobile()` to
roll. Positions and sex are range-checked on the base branch (commit
`7afcd29`).

### Objects (`.obj`)

```
#<vnum>
<keywords>~                  an empty keyword list is fatal
<short description>~
<long description>~
<action description>~
<type> <extra x4> <wear x4> <affect x4>
<value0> <value1> <value2> <value3>
<weight> <cost> <rent per day> <level>
E / A <location> <modifier> / T <trigger vnum>   zero or more
```

### Zones (`.zon`)

```
#<zone vnum>
<builders>~
<name>~
<bot> <top> <lifespan> <reset mode> [<flags x4> <min level> <max level>]
<command> <if-flag> <arg1> <arg2> <arg3>   comment
...
S
$
```

`load_zones()` counts the command lines first and allocates the command
array, then parses each command. Lines starting with `*` are comments. The
commands and their arguments are listed in
[overview.md](overview.md#zone-resets); arguments are vnums on disk and
rnums after `renum_zone_table()`. `V` commands carry two strings
(variable name and value) in `sarg1`/`sarg2`, which `destroy_db()` frees.

### Shops, quests and triggers

Shops (`.shp`) are loaded by `shop.c:boot_the_shops`, quests (`.qst`) by
`quest.c:parse_quest`, triggers (`.trg`) by `dg_db_scripts.c:parse_trigger`.
See [gameplay.md](gameplay.md) and [dg-scripts.md](dg-scripts.md).

## Text files and help

`db.c:file_to_string_alloc(name, &global)` loads a whole text file into a
fresh string:

- `file_to_string()` reads it into a 48 KB stack buffer, converting line
  endings to `\r\n`; a file that does not fit, or cannot be opened, fails
  and the old text stays;
- `@` codes become `\t`;
- the old string is freed and replaced.

If any descriptor's pager is showing the current text, the function returns
-1 before reading, so the text is not reloaded and `reload` reports "Cannot
read" even though the file is fine. The copy-and-repaginate loop that
follows is unreachable for the same reason. (Branch
`audit-deferred-db-loaders`, commit `af7af36`, removes the early return so
the pager keeps a private copy.)

`do_reboot` (the `reload` command, `LVL_IMPL`) reloads all text files or one,
and `reload xhelp` frees and reloads the help table. The help index lists
`.hlp` files; each entry is a keyword line, the text, and a `#<level>` line;
`$` ends a file. `load_help()` builds `help_table[]` with one slot per
keyword; the slots of one entry share its text (`duplicate` counts the extra
slots, so only the first is freed). The table is sorted by keyword for
`search_help()`.

## Player index and player files

### The index

`lib/plrfiles/index` has one line per character:

```
<idnum> <lower-case name> <level> <flags> <last logon time>
...
~
```

`players.c:build_player_index` reads it into `player_table[]` at boot and
sets `top_idnum` to the largest idnum. The table maps names to idnums and
file names. `get_ptable_by_name()`, `get_id_by_name()` and
`get_name_by_id()` search it linearly. Index flags are `PINDEX_DELETED`,
`PINDEX_NODELETE`, `PINDEX_SELFDELETE` and `PINDEX_NOWIZLIST`.

`create_entry(name)` adds a slot (or reuses the slot of a deleted character
with that name) and zeroes it. `save_char()` updates the level, last logon
and flags of the character's slot and calls `save_player_index()` when they
change. `remove_player(pos)` deletes all of a character's files (player
file, rent file, text file, script variables), removes the slot and saves
the index. `clean_pfiles()` applies the `pclean_criteria[]` table from
`config.c` at boot when `auto_pwipe` is set.

A character exists only if its name is in the index: `load_char()` looks
the name up there first, and `nanny()` treats any name `load_char()` cannot
load as a new character.

### Player files

`lib/plrfiles/X-Y/<name>.plr` is a tagged text file. Each line is a
four-character tag, a colon and a value:

```
Name: Joe
Pass: $argon2id$v=19$m=65536,t=2,p=1$...
Levl: 12
Id  : 7
Act : 0 0 0 0
...
Skil:
<skill number> <percent>
0 0
Affs:
<spell> <duration> <modifier> <location> <bitvector x4>
0 0 0 0 0 0 0 0
```

`save_char()` writes the tags `Name Pass Titl Desc PfIn PfOt Sex Clas Levl
Id Brth Plyd Last Lmot Lnew Host Hite Wate Alin Act Aff Pref Wimp Frez Invs
Room Badp Lern Hung Thir Drnk Hit Mana Move Str Int Wis Dex Con Cha Ac Gold
Bank Exp EgAP EgXP EgHP EgMP Hrol Drol Olc Page ScrW Qstp Qcnt Qest Qcur
Trig Skil Affs`, then aliases and script variables. `Desc` is a
`~`-terminated block. Skills are written only for characters below
`LVL_IMMORT`. A numeric field equal to its default from
`pfdefaults.h` (`PFDEF_*`) is not written, and `load_char()` starts every
field at its default. **Changing a `PFDEF_*` value therefore changes the
value of every existing player who had the old default.**

`players.c:load_char` dispatches on the tag. Malformed numbers are logged
with the file and tag and read as 0. Room numbers are vnums.

`players.c:save_char` must store raw values, not values modified by
equipment and spells. It removes every worn item (keeping them in a local
array) and every affect, writes the file, then re-equips and re-applies
them. The file is written to a temporary file (`<name>.plr.tmpXXXXXX` from
`mkstemp()`); only after `fflush()` and `fclose()` succeed is it renamed
over the real file. A failure is reported with `TB_ERROR_ERRNO` and the old
file is left untouched. `save_char()` does nothing for NPCs or characters
without an index position (`GET_PFILEPOS < 0`).

`NO_EXTRANEOUS_TRIGGERS` is defined in `dg_scripts.h`, so the remove and
wear triggers that `save_char()` would otherwise fire while unequipping and
re-equipping are compiled out.

## Rent and crash files

`objsave.c` saves a player's objects to `lib/plrobjs/X-Y/<name>.objs`.

### Format

```
<rent code> <time saved> <cost per day> <gold> <bank> 0
#<vnum>
Loc : <location>
Vals: ...  Flag: ...  Name: ...  Shrt: ...  Desc: ...  ADes: ...~
Type: ...  Wght: ...  Cost: ...  Rent: ...  Perm: ...  Wear: ...
Aff : <slot> <location> <modifier>
EDes:
<keywords>~
<text>~
<blank line>
...
$~
```

Rent codes are `RENT_CRASH` (1), `RENT_RENTED` (2), `RENT_CRYO` (3),
`RENT_FORCED` (4) and `RENT_TIMEDOUT` (5). `objsave_save_obj_record()` writes
an object as a difference from its prototype: it creates a fresh prototype
instance and writes only the fields that differ (the extra, affect and wear
flag lines are always written because the comparison compares array
addresses). An object without a prototype (vnum `NOTHING`) is written in
full.

`Loc` encodes where the object was: 1 to `NUM_WEARS` for an equipment slot
(slot + 1), 0 or absent for the inventory, and -1, -2, ... for the contents
of a container at that depth. `Crash_save()` writes a container's contents
before the container. On load, `handle_obj()` keeps up to `MAX_BAG_ROWS`
(5) levels of pending contents and puts them into the next container read
at the level above; deeper nesting is not restored.

### Savers

| Function | When | Rent code | What happens to the objects |
| --- | --- | --- | --- |
| `Crash_crashsave()` | autosave, entering the game, after many object changes (`PLR_CRASH`) | `RENT_CRASH` | stay on the character |
| `Crash_idlesave()` | idle players being rented out (`limits.c`) | `RENT_TIMEDOUT` | extracted |
| `Crash_rentsave()` | renting at a receptionist, copyover | `RENT_RENTED` | extracted |
| `Crash_cryosave()` | cryo-renting | `RENT_CRYO` | extracted; the cost is taken from gold at once and `PLR_CRYO` is set |
| `Crash_save_all()` | every autosave and at shutdown | | crash-saves and `save_char()`s every playing character flagged `PLR_CRASH` |

The `receptionist` and `cryogenicist` special procedures (`gen_receptionist`)
compute the cost (`Crash_offer_rent()`), refuse unrentable objects
(`ITEM_NORENT`, a negative rent cost, keys, objects without a prototype) and
call the savers.

### Loading

`Crash_load()` runs from `enter_player_game()`. It reads the header,
charges rent for the days since a `RENT_RENTED` or `RENT_TIMEDOUT` save
(a player who cannot pay from gold and bank loses the objects), parses
the records with `objsave_parse_objects()`, and places them with
`handle_obj()` and `auto_equip()` (re-equipping into the saved slot if it is
still valid). The file is then rewritten as a crash save.

`update_obj_file()` runs at boot unless `-q` or `-m` is given and calls
`Crash_clean_file()` for every indexed player: crash, forced and timed-out
files older than `CONFIG_CRASH_TIMEOUT` days and rent files older than
`CONFIG_RENT_TIMEOUT` days are deleted. `Crash_delete_crashfile()` (on
extraction) deletes the file only when it holds a crash save.

On the base branch every saver opens the live file with `fopen("w")`, so a
failure in the middle leaves a truncated file, and the rent savers extract
equipment as soon as each slot is written. Branch
`audit-deferred-player-load` stages saves in a temporary file and commits
them atomically (commit `7f52a39`) and extracts objects only after the file
commits (commit `fec24bf`); `worktree-wf_c613c2c5-f9f-26` has the staging
change as commit `ce6ffba`.

## Houses

`lib/etc/hcontrol` is a raw `fwrite()` of the `house_control[]` array of
`struct house_control_rec` (`house.h`): house vnum, atrium vnum, exit
direction, build time, mode, owner idnum, guest count and up to
`MAX_GUESTS` (10) guest idnums, last payment, and spare fields. The layout
depends on the compiler and platform (`time_t`, `long` and padding), so the
file is not portable between builds with different ABIs. At most
`MAX_HOUSES` (100) houses exist.

`House_boot()` reads the records and keeps those whose owner is in the
player index, whose room and atrium exist, whose vnum is not already a
house and whose exit leads to the atrium. It sets `ROOM_HOUSE`,
`ROOM_PRIVATE` and `ROOM_ATRIUM`, loads each house's objects, and then
rewrites `hcontrol` with only the records it kept. On the base branch the
dropped records are not reported, so a boot with a damaged player index
silently deletes every house, and the guest count is not checked against
`MAX_GUESTS`. Branch `audit-deferred-player-load` reports dropped records
and keeps the file (commit `a8cd507`), rejects bad guest counts (commit
`e1d5e9e`) and stages `hcontrol` saves (commit `9240fba`).

Each house's contents are saved to `lib/house/<vnum>.house` with
`objsave_save_obj_record()` (no rent header). `House_save()` writes every
object with location 0, and `House_load()` puts every object on the floor,
so the contents of containers come back beside their container, not in it.
`obj_to_room()` sets `ROOM_HOUSE_CRASH` on a house room whenever something
is dropped there, and `House_save_all()` (autosave) saves flagged houses.

## Mail

`lib/etc/plrmail` holds every message:

```
### <recipient idnum> <sender idnum> <time sent>
<body>~
```

- `scan_file()` (boot) reads every record to check the file, creating it if
  it does not exist; failure disables mail (`no_mail`).
- `has_mail(id)` scans the whole file.
- `store_mail()` appends one record.
- `read_delete(id)` returns the first message for `id` and rewrites the
  file without it through `plrmail_tmp` and `rename()`.
- The `postmaster` special procedure sends, checks and receives mail;
  received mail becomes a note object.

The body is read with `fread_string()`, so a truncated last message calls
`exit(1)` during `scan_file()` at boot or `has_mail()` at login. Branch
`audit-deferred-player-load` (commit `40e9275`) and
`worktree-wf_c613c2c5-f9f-26` (commit `7adff57`) make it a reported error.

## Bulletin boards

`boards.c` serves the board objects listed in `board_info[]` through the
`gen_board` special procedure. Each board has its own file
(`lib/etc/board.mortal`, `board.immortal`, ...). The file is binary: an
`int` message count, then for each message a raw `struct board_msginfo`
followed by the heading and body bytes. The struct contains a `char *`, so
the file layout depends on pointer size; the stored pointer value is
ignored on load. A count outside `1..MAX_BOARD_MESSAGES` or a short read
resets (empties) the board.

Saves write `<file>.tmp` and rename it over the board file, using a `.bak`
copy where `rename()` cannot replace an existing file. All boards share one
pool of message bodies (`msg_storage[]`). Boards are initialised the first
time any board is used (`init_boards()`); on the base branch a missing
board object then calls `exit(1)` in the running server. Branch
`worktree-wf_c613c2c5-f9f-42` (commit `85ce26b`) initialises boards in
`boot_db()` and reports missing objects.

## Ideas, bugs and typos

`ibt.c` keeps one linked list per kind, loaded at boot from
`lib/misc/ideas`, `bugs` and `typos`. Records are keyword lines (`Text`,
`Body`, `Name`, `Notes` as `~`-terminated strings, `IdNum`, `Dated`,
`Level`, `Room`, `Flags`) ending with `End`. `save_ibt_file()` rewrites the
whole file with `fopen("w")`. On the base branch a malformed record can stop
the boot or crash it; branch `worktree-wf_c613c2c5-f9f-42` (commit
`fae8af1`) drops malformed records with a report instead. A new entry is
linked into the list before its body is written in the string editor and
removed again if the player aborts (`clean_ibt_list()`).

## Configuration

The settings that `cedit` can change live in `config_info` (`struct
config_data`, `structs.h`) and are read through the `CONFIG_*` macros
(`utils.h`).

1. `config.c` holds the compiled-in defaults (`pk_allowed`, `max_playing`,
   rent settings, start rooms, `DFLT_PORT`, `DFLT_DIR`, the menu and
   welcome texts, ...) and the `pclean_criteria[]` table. It also holds
   flags that are not in `config_info`, such as `auto_pwipe`,
   `selfdelete_fastwipe`, `bitwarning` and `bitsavetodisk`.
2. `db.c:load_config` copies the defaults into `config_info`
   (`load_default_config()`, which `strdup()`s every string), then reads
   `CONFIG_CONFFILE` (default `etc/config`, tried relative to the current
   directory and then under `CONFIG_DFLT_DIR`). Each line is `tag = value`;
   unknown tags are ignored, and numbers are read with
   `tb_parse_int_or_zero()`, so a non-number becomes 0 without a report.
   A missing file means defaults.
3. `cedit` edits a copy (`OLC_CONFIG`), copies it back
   (`cedit_save_internally()`), and `cedit.c:save_config` writes every
   setting to `<file>.tmp` and replaces the file with
   `tb_replace_file_checked()`.

`load_config()` runs before the log file is opened and before `chdir()`,
so its errors go to standard error.

Strings in `config_info` are owned by it and freed at shutdown by
`oasis_delete.c:free_strings(&config_info, OASIS_CFG)`. The `-o` option
breaks that rule; see [failure behaviour](#failure-behaviour).

## Failure behaviour

The project rule is *fail fast, fail loudly*: a rejected or repaired value
must produce a structured diagnostic, and a failed save must not destroy the
last good copy. The table records what the base branch actually does.

| Component | Malformed input | Read or write failure | Save is atomic |
| --- | --- | --- | --- |
| World loaders (`index_boot`, `parse_*`, `load_zones`) | `exit(1)` with a `SYSERR` naming the file or record; some values repaired with a log (`check_object`, sectors); bad zone commands disabled | missing index file: `exit(1)`; data file missing in the counting pass: reported and skipped, in the loading pass: `exit(1)` | OLC savers: temp file + replace ([olc.md](olc.md)) |
| `fread_string()` | end of file before `~`, or more than 48 KB: `exit(1)`, also at run time (mail, player descriptions) | | |
| Text files | too large: kept old text, `-1` | kept old text, `-1` | `tedit` writes in place |
| Player index | a malformed line is logged and **ends the read**: every later player is missing from the table until the next boot, and their names look free | missing file: empty table ("First new char will be IMP") | no (`fopen("w")`); staged on `audit-deferred-player-load` (`8e5b5a5`) |
| Player files | bad numbers logged and read as 0 | unreadable file: `load_char()` returns -1 and the name is offered for a new character | yes (`mkstemp` + `rename`) |
| Rent/crash files | diagnostics name the file, owner, line and vnum | live file truncated first | no; staged on `audit-deferred-player-load` |
| `hcontrol` | invalid records silently dropped and the file rewritten | | no; staged on `audit-deferred-player-load` |
| Mail | bad header ends the scan; scan failure disables mail; truncated body: `exit(1)` | | delete path: temp + `rename` |
| Boards | bad count or short read: board reset | | yes (temp + `rename`, `.bak` fallback) |
| IBT files | can stop or crash the boot | `fopen("w")` failure reported | no |
| Ban list | bad lines skipped | | no |
| Configuration | unknown tags ignored, non-numbers become 0 | missing file: defaults | yes (`tb_replace_file_checked`) |
| `etc/time` | | write failure logged | no |

Points to keep in mind when changing persistence code:

- **Write to a temporary file and replace.** Use
  `sysdep.h:tb_replace_file_checked()` or the `mkstemp()` + `rename()`
  pattern from `save_char()`, check `ferror()`, `fflush()` and `fclose()`
  before committing, and leave the old file in place on failure.
- **Report through the structured reporters**, naming the file, the record
  (vnum, idnum, player name) and the field. Do not log raw player text.
- **Do not add new `exit(1)` calls on paths a running server reaches.**
  Several existing ones (`fread_string()`, `extract_char_final()`,
  `char_from_room()`) take the whole game down for one bad record.
- **Store vnums and idnums, never rnums or pointers.** The board and house
  formats show what happens to raw structs.

### Known problems

- `players.c:build_player_index` stops at the first malformed index line
  and keeps only the records before it. The later players cannot log in,
  their names are offered to new characters (whose saves replace the old
  player files), and the next `save_player_index()` writes the truncated
  table. Fixed on `audit-deferred-player-load` (commit `3d6e0cc`).
- `comm.c:main` stores the `-f` and `-o` arguments (pointers into `argv`)
  in `CONFIG_CONFFILE` and `CONFIG_LOGNAME`. `main()` later calls
  `free(CONFIG_CONFFILE)`, and `free_strings(&config_info, OASIS_CFG)` (at
  shutdown) and `cedit_save_internally()` free `CONFIG_LOGNAME`, so a server
  started with `-f` or `-o` frees memory it does not own; `-o` also leaks
  the `LOGNAME` string read from the configuration. `autorun` passes neither
  option. No branch changes this.
- `db.c:index_boot` reads index lines with an unbounded `fscanf("%s")`
  into a `PATH_MAX - 100` buffer. Fixed on `audit-deferred-db-loaders`
  (commit `4e96f05`).
- `db.c:setup_dir` builds its diagnostic context with
  `GET_ROOM_VNUM(room) + 1` for a room that is not yet counted in
  `top_of_world`, so its messages name the wrong room.
