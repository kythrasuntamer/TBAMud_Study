# tbaMUD architecture documentation

These documents explain what the tbaMUD engine does and the rules its code
relies on: who owns each object, how long it lives, what may be `NULL`, who
frees what, and which inputs come from whom. They are written for
maintainers, in the spirit of Jeremy Elson's CircleMUD 3.0 documentation, but
describe the code as it is in this repository (a CircleMUD 3.x / tbaMUD
derivative with libsodium passwords, structured diagnostics and a hardened
build).

The code is the authority. These documents name functions as
`file.c:function` instead of line numbers, because line numbers move.

## Reading order

| # | Document | Read it for |
| --- | --- | --- |
| 1 | [overview.md](overview.md) | Boot and shutdown, the game loop and pulse timing, descriptors, characters, objects and rooms, rnums and vnums, extraction and the purge list, zone resets, the event systems |
| 2 | [foundations.md](foundations.md) | `structs.h` types, the `utils.h` macro families, `sysdep.h` portability and `tb_*` wrappers, structured diagnostics, zmalloc, sanitizers, Valgrind, fuzzing, tests, the `src/util` tools |
| 3 | [networking.md](networking.md) | Sockets and descriptors, input and output buffering, telnet/MSDP/MXP and colour, the login state machine, the command interpreter, aliases, bans, the string editor and pager |
| 4 | [persistence.md](persistence.md) | World files and how they load, player files and the index, rent files, houses, mail, boards, ideas/bugs/typos, configuration, and what happens when a load or save fails |
| 5 | [dg-scripts.md](dg-scripts.md) | The DG Scripts trigger engine: triggers, dispatch, variables, waits, the UID table, callback lifetime rules |
| 6 | [olc.md](olc.md) | OasisOLC: the editors, the scratch-copy lifecycle, saving to memory and disk, renumbering |
| 7 | [gameplay.md](gameplay.md) | Commands, combat, magic, classes, mobiles, shops, quests and the other game rules |

Also read:

- `docs/maintainer-safety-guide.md`: how to report errors, the DG callback
  rules (snapshot identities, re-validate after every script), buffer rules
  and test expectations. Required before changing trigger dispatch, the UID
  table, `act()` or string builders.
- `docs/macro-audit.md` and `docs/memory-safety-audit.md`: audit history.
- `doc/`: the original CircleMUD/tbaMUD manuals. `building.txt` (world file
  formats), `coding.txt` (adding commands, spells and specials), `act.txt`
  (`act()` codes), `color.txt`, `admin.txt`, `utils.txt`, `syserr.txt`,
  `dg_events.txt`, `testing.md` and `README.CMAKE.md` are still useful;
  where they disagree with these documents, check the code.

## Conventions used in these documents

- **Base branch.** The text describes branch
  `audit/macro-inventory-and-dg-followup-20261027`. Many safety fixes from
  the October 2026 audit live on other branches (`audit-fix-*`,
  `audit-deferred-*`, `audit-sanitizer-infra`, `worktree-wf_c613c2c5-f9f-*`)
  that were not merged when this was written. Where a document mentions
  one, it names the branch and commit; read the commit before relying on it.
- **Who controls an input.** "Remote client" means anyone who can open a
  connection, before any password check. "Player" means a logged-in
  character. "Immortal" means level 51 (`LVL_IMMORT`) or higher; "builder"
  means an immortal with OLC permission for a zone. "World files" are
  written by builders and administrators and loaded at boot.
- **rnum / vnum.** A vnum is the number a builder assigns; an rnum is an
  array index valid only until the array changes. See
  [overview.md](overview.md#real-numbers-and-virtual-numbers).
- **Fail fast, fail loudly.** A guard that rejects, repairs or skips
  something must emit a structured diagnostic (`TB_ERROR_*`), and a failed
  save must keep the last good file. The documents point out places that do
  not yet meet this rule.

## Source file map

Every file in `src/` and `src/util/`, grouped by subsystem. A `.c` file and
its header share a row.

### Process, networking and command input

| Files | What they hold | Document |
| --- | --- | --- |
| `comm.c`, `comm.h` | `main()`, command-line options, copyover recovery, the listening socket, `game_loop()` and `heartbeat()`, input framing (`process_input`, history, `^old^new`, `--`), output buffering (`vwrite_to_output`, the small/large buffer pool, `process_output`), `close_socket()`, signal handlers, the `send_to_*` family, `act()`/`perform_act()`, log setup, per-second MSDP updates | [networking.md](networking.md), [overview.md](overview.md) |
| `protocol.c`, `protocol.h` | KaVir's protocol snippet: telnet negotiation (TTYPE, NAWS, CHARSET, MSDP, MSSP, MSP, MXP, ATCP), in-band/out-of-band input splitting (`ProtocolInput`), colour/MXP/unicode expansion (`ProtocolOutput`), the MSDP variable table, MSSP, copyover state | [networking.md](networking.md) |
| `telnet.h` | the BSD `arpa/telnet.h` constants (`IAC`, `WILL`, `TELOPT_*`), used when the system header is missing | [networking.md](networking.md) |
| `interpreter.c`, `interpreter.h` | the master command table `cmd_info[]`, `command_interpreter()`, special-procedure dispatch, aliases, the argument parsers (`one_argument` family), the login state machine `nanny()`, Argon2id password hashing, the login rate limit, duplicate-login handling, `enter_player_game()` | [networking.md](networking.md) |
| `ban.c`, `ban.h` | site bans (`badsites`), `isbanned()`, `do_ban`/`do_unban`, the invalid-name list (`xnames`) and `valid_name()` | [networking.md](networking.md) |
| `modify.c`, `modify.h` | the string editor core (`string_write`, `string_add`), `smash_tilde`, `parse_at`/`parse_tab`, the pager (`page_string`, `show_string`), `do_skillset` | [networking.md](networking.md) |
| `improved-edit.c`, `improved-edit.h` | the `/`-command line editor (list, insert, delete, edit, replace, format, toggle colour codes) and the text formatter | [networking.md](networking.md) |

### Core types, utilities and portability

| Files | What they hold | Document |
| --- | --- | --- |
| `structs.h` | the core data model: index types and sentinels, every persisted flag and type constant, buffer sizes, pulse constants, and the structs for objects, rooms, characters, descriptors, indexes and configuration | [foundations.md](foundations.md), [overview.md](overview.md) |
| `utils.c`, `utils.h` | the macro families (`GET_*`, `IS_*`, flag tests, `CREATE`/`RECREATE`, list macros, `CONFIG_*`), the structured diagnostics (`TB_ERROR_*`, breadcrumbs, correlation, parse context), `tb_buf`, checked allocation, logging (`log`, `mudlog`), `rand_number`/`dice`, `get_line`, `get_filename`, `sprintbit`/`sprinttype`, followers, time and text helpers | [foundations.md](foundations.md) |
| `sysdep.h` | system includes and portability, strict and legacy number parsers, the allocation-free `TB_REPORT_*` reporter, the `tb_*` stdio wrappers and checked variants, `tb_snprintf`, `tb_strftime`, `tb_replace_file_checked`, test failpoints, library prototypes | [foundations.md](foundations.md) |
| `random.c` | the Park-Miller random number generator behind `circle_srandom()`/`circle_random()` | [foundations.md](foundations.md) |
| `constants.c`, `constants.h` | name tables for every enumerated value (`"\n"`-terminated) and the stat tables (`str_app`, `con_app`, `movement_loss`, `rev_dir`, ...) | [foundations.md](foundations.md) |
| `bsd-snprintf.c`, `bsd-snprintf.h` | fallback `snprintf`/`vsnprintf`, compiled only when the C library lacks them | [foundations.md](foundations.md) |
| `screen.h` | legacy ANSI colour strings and the per-character `CC*`/`CB*`/`Q*` colour macros | [foundations.md](foundations.md) |
| `zmalloc.c`, `zmalloc.h` | the `MEMORY_DEBUG` allocation monitor: guard pads, leak and double-free report at shutdown | [foundations.md](foundations.md) |
| `lists.c`, `lists.h` | the generic doubly linked list, iterators and the non-reentrant `simple_list()` | [overview.md](overview.md) |
| `config.c`, `config.h` | compiled-in defaults for the configuration and the player-file cleanup table (`pclean_criteria`) | [persistence.md](persistence.md) |
| `pfdefaults.h` | default values of player-file fields; a field equal to its default is not written | [persistence.md](persistence.md) |

### World database and persistence

| Files | What they hold | Document |
| --- | --- | --- |
| `db.c`, `db.h` | the global tables and lists, `boot_db()`/`boot_world()`, `index_boot()` and the record parsers (rooms, mobiles, objects, zones, help), string readers (`fread_string`), zone resets (`zone_update`, `reset_zone`), instantiation (`read_mobile`, `read_object`), `free_char`/`free_obj`, `destroy_db()`, text files, vnum-to-rnum searches, `load_config()`; `db.h` holds the `lib/` paths and the zone and index structs | [persistence.md](persistence.md), [overview.md](overview.md) |
| `players.c` | the player index (`player_table`), ASCII player files (`load_char`, `save_char` with staged commit), `create_entry`, `remove_player`, `clean_pfiles` | [persistence.md](persistence.md) |
| `objsave.c` | rent, crash, idle and cryo object files, the object record reader and writer, re-equipping at login, the receptionist and cryogenicist | [persistence.md](persistence.md) |
| `house.c`, `house.h` | player houses: the binary `hcontrol` table, house object files, `hcontrol` and `house` commands, `House_can_enter()` | [persistence.md](persistence.md) |
| `mail.c`, `mail.h` | mudmail in `etc/plrmail`, the postmaster special procedure | [persistence.md](persistence.md) |
| `boards.c`, `boards.h` | bulletin boards (`gen_board`), their binary files and shared message storage | [persistence.md](persistence.md) |
| `ibt.c`, `ibt.h` | ideas, bugs and typos: lists, files, commands and the `ibtedit` editor | [persistence.md](persistence.md) |

### World primitives and events

| Files | What they hold | Document |
| --- | --- | --- |
| `handler.c`, `handler.h` | name matching (`isname`, `fname`), affects, moving characters and objects (`char_to_room`, `obj_to_char`, `equip_char`, ...), `extract_obj()`, deferred `extract_char()` and `extract_pending_chars()`, visibility-aware finders (`get_char_vis`, `generic_find`), money objects, groups; `handler.h` also defines the binary last-login record | [overview.md](overview.md), [gameplay.md](gameplay.md) |
| `dg_event.c`, `dg_event.h` | the global event queue: `event_create`, `event_cancel`, `event_process`, the bucketed priority queue | [overview.md](overview.md), [dg-scripts.md](dg-scripts.md) |
| `mud_event.c`, `mud_event.h` | typed events with owners (world, descriptor, character, room), `NEW_EVENT`, `free_mud_event`, `event_countdown` | [overview.md](overview.md) |

### Gameplay

| Files | What they hold | Document |
| --- | --- | --- |
| `act.h` | prototypes and `SCMD_*` subcommands for the `act.*.c` command files | [gameplay.md](gameplay.md) |
| `act.comm.c` | say, tell, whisper, channels, write, page, group say; channel history | [gameplay.md](gameplay.md) |
| `act.informative.c` | look, examine, score, inventory, equipment, who, where, help, time, weather, toggles, `commands`, `whois`, `scan` | [gameplay.md](gameplay.md) |
| `act.item.c` | get, put, drop, give, donate, junk, drink, eat, pour, wear, wield, remove, sacrifice | [gameplay.md](gameplay.md) |
| `act.movement.c` | moving through exits (`do_simple_move`, `perform_move`), doors and locks, enter/leave, positions, furniture, follow | [gameplay.md](gameplay.md) |
| `act.offensive.c` | assist, hit, kill, backstab, order, flee, bash, rescue, kick, bandage, whirlwind | [gameplay.md](gameplay.md) |
| `act.other.c` | quit, save, sneak, hide, steal, practice, title, groups, split, use/quaff/recite, toggles, happy hour, endgame advancement | [gameplay.md](gameplay.md) |
| `act.social.c` | socials (`do_action`, `do_gmote`), `create_command_list()` which merges socials into the command table | [gameplay.md](gameplay.md) |
| `act.wizard.c` | immortal and builder commands: goto, stat, load, purge, set, force, snoop, switch, shutdown, copyover, `show`, `zcheck`, last-login file, `oset` | [gameplay.md](gameplay.md) |
| `fight.c`, `fight.h` | the combat engine: `set_fighting`, `hit`, `damage`, `raw_kill`, `die`, corpses, experience, damage messages, `perform_violence()` | [gameplay.md](gameplay.md) |
| `magic.c` | saving throws, `affect_update()`, and the spell routine templates (damage, affects, groups, areas, summons, points, objects, rooms) | [gameplay.md](gameplay.md) |
| `spells.c`, `spells.h` | manual spells (recall, teleport, summon, locate object, charm, identify, enchant weapon, ...); spell and skill numbers, target and routine flags | [gameplay.md](gameplay.md) |
| `spell_parser.c` | the spell table (`spell_info`), `do_cast`, `cast_spell`, `call_magic`, staffs, wands, scrolls and potions | [gameplay.md](gameplay.md) |
| `class.c`, `class.h` | classes: parsing, guilds, practice parameters, saving throws, THAC0, starting stats, levelling, spell levels, titles | [gameplay.md](gameplay.md) |
| `limits.c` | regeneration, experience and levels, hunger and thirst, idle handling, `point_update()`, gold and bank helpers, `run_autowiz()` | [gameplay.md](gameplay.md) |
| `mobact.c` | mobile AI each `PULSE_MOBILE`: special procedures, hunting, scavenging, wandering, aggression, memory, charm | [gameplay.md](gameplay.md) |
| `graph.c`, `graph.h` | breadth-first path finding for `track` and mobile hunting | [gameplay.md](gameplay.md) |
| `weather.c` | game time and the weather state machine | [gameplay.md](gameplay.md) |
| `shop.c`, `shop.h` | shopkeepers: loading `.shp` files, buy, sell, value, list, prices, keeper messages | [gameplay.md](gameplay.md) |
| `quest.c`, `quest.h` | autoquests: loading `.qst` files, questmasters, quest commands, completion and rewards; `quest.h` also declares `qedit` and `genqst` | [gameplay.md](gameplay.md) |
| `spec_assign.c` | assigns special procedures to mobile, object and room vnums at boot | [gameplay.md](gameplay.md) |
| `spec_procs.c`, `spec_procs.h` | the classic special procedures (guilds and practice, dump, mayor, pet shops, bank, guards, janitor, fido, ...) | [gameplay.md](gameplay.md) |
| `castle.c` | special procedures for King Welmar's castle zone | [gameplay.md](gameplay.md) |
| `asciimap.c`, `asciimap.h` | the ASCII map and the automap beside room descriptions | [gameplay.md](gameplay.md) |

### DG Scripts

| Files | What they hold | Document |
| --- | --- | --- |
| `dg_scripts.c`, `dg_scripts.h` | the script interpreter `script_driver()`, expression evaluation, control flow, `wait`, the UID lookup table, attach/detach commands, script variable saving; the trigger type bits and script structs | [dg-scripts.md](dg-scripts.md) |
| `dg_triggers.c` | every trigger dispatcher (greet, command, speech, act, fight, death, load, wear, ...) | [dg-scripts.md](dg-scripts.md) |
| `dg_variables.c` | variable storage and `%var.field%` substitution | [dg-scripts.md](dg-scripts.md) |
| `dg_db_scripts.c` | loading trigger prototypes and attaching them to mobiles, objects and rooms | [dg-scripts.md](dg-scripts.md) |
| `dg_handler.c` | freeing triggers and scripts, deferred retirement while scripts run | [dg-scripts.md](dg-scripts.md) |
| `dg_comm.c` | script message helpers (`sub_write`, `send_to_zone`) | [dg-scripts.md](dg-scripts.md) |
| `dg_misc.c` | script casting, affects and damage, `valid_dg_target()` | [dg-scripts.md](dg-scripts.md) |
| `dg_mobcmd.c`, `dg_objcmd.c`, `dg_wldcmd.c` | the mobile, object and room script commands | [dg-scripts.md](dg-scripts.md) |
| `dg_olc.c`, `dg_olc.h` | the trigger editor (`trigedit`) and the trigger-list editor shared by medit, oedit and redit | [dg-scripts.md](dg-scripts.md), [olc.md](olc.md) |

### Online creation (OasisOLC)

| Files | What they hold | Document |
| --- | --- | --- |
| `oasis.c`, `oasis.h` | shared OLC support: the per-descriptor `oasis_olc_data`, `cleanup_olc()`, zone permission checks, the `OLC_*` accessors and every editor's prototypes | [olc.md](olc.md) |
| `oasis_copy.c` | `rcopy`/`ocopy`/`mcopy`/`scopy`/`tcopy`, `dig`, buildwalk | [olc.md](olc.md) |
| `oasis_delete.c` | `free_strings()` for rooms, exits and configuration | [olc.md](olc.md) |
| `oasis_list.c` | `rlist`, `olist`, `mlist`, `tlist`, `slist`, `qlist`, `zlist` | [olc.md](olc.md) |
| `genolc.c`, `genolc.h` | the save list, string helpers, extra-description helpers, `sprintascii`, zone export | [olc.md](olc.md) |
| `genwld.c`, `genwld.h` | adding, deleting, copying and saving rooms (with renumbering) | [olc.md](olc.md) |
| `genmob.c`, `genmob.h` | adding, deleting and saving mobile prototypes | [olc.md](olc.md) |
| `genobj.c`, `genobj.h` | adding, deleting and saving object prototypes; the `oset_*` helpers | [olc.md](olc.md) |
| `genzon.c`, `genzon.h` | creating and saving zones, reset-command list helpers, world index files | [olc.md](olc.md) |
| `genshp.c`, `genshp.h` | copying, adding and saving shops | [olc.md](olc.md) |
| `genqst.c` | copying, adding, deleting and saving quests | [olc.md](olc.md) |
| `redit.c`, `oedit.c`, `medit.c`, `zedit.c`, `sedit.c`, `qedit.c` | the room, object (and live-object `iedit`), mobile, zone, shop and quest editors | [olc.md](olc.md) |
| `cedit.c` | the configuration editor and `save_config()` | [olc.md](olc.md), [persistence.md](persistence.md) |
| `aedit.c` | the social editor and `astat` | [olc.md](olc.md) |
| `hedit.c`, `hedit.h` | the help editor, `helpcheck`, `hindex` | [olc.md](olc.md) |
| `tedit.c` | the text-file editor (news, motd, credits, ...) | [olc.md](olc.md) |
| `msgedit.c`, `msgedit.h` | loading, editing and saving combat messages | [olc.md](olc.md) |
| `prefedit.c`, `prefedit.h` | the player preference editor | [olc.md](olc.md) |

### Stand-alone tools (`src/util/`)

| File | Purpose | Document |
| --- | --- | --- |
| `asciipasswd.c` | print a crypt()-hashed `Pass:` line for a player file | [foundations.md](foundations.md#stand-alone-tools-srcutil) |
| `autowiz.c` | build the wizlist and immlist from the player index; signals the server | same |
| `plrtoascii.c` | convert CircleMUD 3.0 binary player files to ASCII player files | same |
| `rebuildAsciiIndex.c` | rebuild `plrfiles/index` from the `.plr` files (built as `rebuildIndex`) | same |
| `rebuildMailIndex.c` | rebuild a mail index from `.ml` files | same |
| `shopconv.c` | convert CircleMUD 2.20 shop files to the 3.0 format | same |
| `sign.c` | a small TCP server that shows a fixed message to every connection | same |
| `split.c` | split standard input into several files | same |
| `wld2html.c` | convert room files into linked HTML pages | same |

`src/util/scheck` is a note saying the syntax checker is now `circle -c`.
The build files (`src/CMakeLists.txt`, `src/util/CMakeLists.txt`,
`src/Makefile.in`, the platform `Makefile.*` and `conf.h.*` files and the
`*.doxyfile` files) are not described here; see
[foundations.md](foundations.md#header-layering) for `conf.h` and
`doc/README.*` for the platform builds.
