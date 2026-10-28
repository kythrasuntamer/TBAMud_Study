# Engine overview

This document explains how the tbaMUD server is put together: how it boots,
how the main loop keeps time, how the in-memory world is modelled, how
things are created and destroyed, and how deferred work (events) is run.
It is written for maintainers who need to change the engine without
breaking its invariants.

tbaMUD is a CircleMUD 3.x derivative (CircleMUD itself descends from
DikuMUD). The server is one process with one thread. All game state lives in
memory; the world is read from `lib/` at boot and written back only when an
editor saves it. The only concurrency is the `select()` loop in `comm.c`,
which multiplexes every socket. Time advances in *pulses* of 0.1 seconds.

Related documents:

- [networking.md](networking.md): sockets, telnet, the login state machine,
  the command interpreter and the string editor.
- [persistence.md](persistence.md): world files, player files, rent, houses,
  mail, boards and configuration on disk.
- [foundations.md](foundations.md): the core types, macro families, safe
  wrappers, structured diagnostics and the test and sanitizer tooling.
- [dg-scripts.md](dg-scripts.md), [olc.md](olc.md),
  [gameplay.md](gameplay.md): the trigger engine, the online editors and the
  game rules.

Function references use the form `file.c:function`. Line numbers are not
used because they move.

## Contents

1. [Process lifetime](#process-lifetime)
2. [The game loop](#the-game-loop)
3. [The data model](#the-data-model)
4. [Real numbers and virtual numbers](#real-numbers-and-virtual-numbers)
5. [Creation, extraction and the purge list](#creation-extraction-and-the-purge-list)
6. [Zone resets](#zone-resets)
7. [Event systems](#event-systems)
8. [Generic lists](#generic-lists)
9. [Where each subsystem lives](#where-each-subsystem-lives)

## Process lifetime

### Command line

`comm.c:main` reads options in two passes. The first pass looks only for
`-f`, because the configuration file must be loaded before anything else
reads a configuration value. Options stop at the first argument that does
not start with `-`; that argument, if present, is the port.

| Option | Effect |
| --- | --- |
| `-f<file>` / `-f <file>` | Configuration file to load instead of `etc/config`. |
| `-o <file>` | Log file instead of standard error. |
| `-d <dir>` | Library directory (default `CONFIG_DFLT_DIR`, normally `lib`). |
| `-c` | Syntax check: load the world, free it, exit. |
| `-m` | Mini-MUD: use `index.mini` files, skip the rent check and houses. |
| `-q` | Quick boot: skip the scan for timed-out rent files. |
| `-r` | Restrict: no new characters (`circle_restrict = 1`). |
| `-s` | No special procedures and no shops. |
| `-C<fd>` | Internal: recover from a copyover using mother socket `<fd>`. |
| `<port>` | TCP port; must be above 1024. |

`autorun` starts the server with `-q` and restarts it according to the exit
code and the `.killscript`, `pause` and `.fastboot` files in the directory
above `lib/`.

### Boot sequence

```
main()
 |- load_config()                 lib/etc/config over config.c defaults
 |- parse options, setup_log()    logfile = file or stderr
 |- chdir(lib)                    every later path is relative to lib/
 |- -c?  boot_world(); destroy_db(); exit
 '- init_game(port)
     |- touch ../.killscript      autorun will not restart a failed boot
     |- circle_srandom(time(0))
     |- get_max_players()         raises RLIMIT_NOFILE, caps max_playing
     |- init_socket(port)         skipped after a copyover (fd inherited)
     |- event_init()              the global DG event queue
     |- init_lookup_table()       DG UID -> entity table
     |- boot_db()                 everything below
     |- signal_setup()
     |- remove ../.killscript
     |- copyover_recover()        only with -C
     |- game_loop(mother_desc)    runs until circle_shutdown
     '- shutdown (see below)
```

`db.c:boot_db` loads the game in this order. The order matters: each step
uses data the earlier steps produced.

1. `reset_time()`: game clock from `etc/time`.
2. Global lists (`global_lists`, `group_list`) and `init_events()`
   (`world_events`).
3. Text files (news, credits, motd, imotd, help front pages, info, wizlist,
   immlist, policies, handbook, background, greetings) through
   `file_to_string_alloc()`.
4. `mag_assign_spells()`: the spell table.
5. `boot_world()`: zones, triggers, rooms, `renum_world()`,
   `check_start_rooms()`, mobiles, objects, `renum_zone_table()`, shops
   (unless `-s`), quests. Zones come first because `parse_room()` assigns
   each room to the zone whose range contains it; triggers come before
   rooms, mobiles and objects because their `T` lines name trigger vnums.
6. Help entries, then `build_player_index()` and, if `auto_pwipe` is set,
   `clean_pfiles()`.
7. Combat messages, socials, `create_command_list()`.
8. Special procedure assignment (unless `-s`): mobiles, shopkeepers,
   objects, rooms, questmasters.
9. `init_spell_levels()`, `sort_commands()`, `sort_spells()`.
10. Mail scan (a failure disables mail with `no_mail = 1`), the ban list,
    the invalid-name list, the idea/bug/typo lists.
11. `update_obj_file()` (unless `-q` or `-m`): removes timed-out rent files.
12. `House_boot()` (unless `-m`): needs objects and the player index.
13. `clean_llog_entries()`, then `reset_zone()` for every zone.

Syntax-check mode (`-c`) calls `boot_world()` only. It does not create the
event queue, the UID table, help, players or zone resets. It is the fastest
way to validate world files, and `tests/CMakeLists.txt` runs it as
`test_world_boot` so the sanitizer builds exercise every world loader and
`destroy_db()`.

### Shutdown and reboot

The `shutdown` command (`act.wizard.c:do_shutdown`) sets `circle_shutdown`.
`shutdown reboot` and `shutdown now` also set `circle_reboot = 2`, which
skips the OLC autosave. When `game_loop()` returns, `init_game()` saves every
player's objects (`Crash_save_all()`), closes every descriptor (which saves
the characters still playing), closes the mother socket, writes pending OLC
changes (`save_all()`) unless `circle_reboot == 2`, saves the game time, and
then either exits with code 52 (reboot; `autorun` restarts) or returns.
Only a normal shutdown returns to `main()`, which frees the world with
`destroy_db()` and the remaining tables, and runs `zmalloc_check()` under
`MEMORY_DEBUG`.

`SIGHUP`, `SIGINT` and `SIGTERM` go to `comm.c:hupsig`, which logs and calls
`exit(1)` from inside the signal handler. Nothing is saved, so progress since
the last autosave is lost. (Branch `worktree-wf_c613c2c5-f9f-25`, commit
`dba8366`, changes the handlers to only set flags.)

### Copyover

`act.wizard.c:do_copyover` restarts the binary without dropping
connections. It writes `copyover.dat` in `lib/`: the boot time on the first
line, then one line per playing descriptor (`fd pref name host protocol`),
then `-1`. It closes every descriptor that is not playing (including those
in OLC), rent-saves and saves each player, changes to the parent directory
and `execl()`s `bin/circle -C<mother fd> <port>`. The descriptors survive the
`exec`. The new process boots normally, skips `init_socket()`, and
`comm.c:copyover_recover` rebuilds a descriptor for each line, reloads the
player file, restores the telnet options with `CopyoverSet()` and puts the
character back in the game with `enter_player_game()`. A line whose file
cannot be loaded closes that connection.

## The game loop

`comm.c:game_loop` runs until `circle_shutdown` is set. One pass:

```
if no descriptors: block in select() on the mother socket only
build the read/write/exception sets (mother + every descriptor)
sleep until the next 0.1 s boundary          (circle_sleep)
select() with a zero timeout                 (poll)
accept at most one new connection            (new_descriptor)
close descriptors in the exception set
read input from readable descriptors         (process_input; close on error)
run at most one queued command per descriptor
flush output to writable descriptors         (process_output)
write a prompt to descriptors that got no output
close descriptors in CON_CLOSE or CON_DISCONNECT
call heartbeat(++pulse) once per elapsed pulse
handle the SIGUSR1/SIGUSR2 flags
```

Command dispatch is where most game code runs. For each descriptor with a
character, the wait state (`GET_WAIT_STATE`) is decremented, and the
descriptor is skipped while it is positive. Otherwise one line is taken from
the input queue, the idle timer is reset, a link-dead character is pulled
back from the void, and the wait state is set to 1. The line then goes to the
first matching consumer:

1. the pager, if the descriptor is paging (`show_string`);
2. the string editor, if `d->str` is set (`string_add`);
3. `nanny()`, if the state is not `CON_PLAYING` (this includes every OLC
   editor);
4. the alias expander and then `command_interpreter()`.

Because the wait state is at least 1 after any command, a descriptor runs at
most one command per pulse (ten per second). Skills add longer lag by setting
a larger wait state. See [networking.md](networking.md) for the input path
and the interpreter.

If a pass takes longer than a pulse, the loop runs `heartbeat()` once for
every missed pulse, capped at 30 seconds of pulses with a `SYSERR`. A
virtual-time watchdog (`SIGVTALRM` every 180 CPU seconds, `checkpointing()`)
aborts the process if no loop pass completed in that time; it is disabled
under `MEMORY_DEBUG`.

### Pulse timing

`OPT_USEC` is 100000, so `PASSES_PER_SEC` is 10 and `n RL_SEC` means `n * 10`
pulses. `comm.c:heartbeat` runs, in this order:

| When | Constant | Work |
| --- | --- | --- |
| Every pulse | | `event_process()` (DG waits and mud events) |
| Every 13 s | `PULSE_DG_SCRIPT` | `script_trigger_check()` (random triggers) |
| Every second | `PASSES_PER_SEC` | `msdp_update()`, tick countdown |
| Every 10 s | `PULSE_ZONE` | `zone_update()` |
| Every 15 s | `PULSE_IDLEPWD` | `check_idle_passwords()` |
| Every 10 s | `PULSE_MOBILE` | `mobile_activity()` |
| Every 2 s | `PULSE_VIOLENCE` | `perform_violence()` (one combat round) |
| Every mud hour (75 s) | `SECS_PER_MUD_HOUR` | `weather_and_time()`, `check_time_triggers()`, `affect_update()`, `point_update()`, `check_timed_quests()` |
| Every minute, if `CONFIG_AUTO_SAVE` | `PULSE_AUTOSAVE` | every `CONFIG_AUTOSAVE_TIME` minutes: `Crash_save_all()`, `House_save_all()` |
| Every 5 minutes | `PULSE_USAGE` | `record_usage()` |
| Every 30 minutes | `PULSE_TIMESAVE` | `save_mud_time()` |
| Every pulse, last | | `extract_pending_chars()` |

A mud hour is 75 real seconds, a mud day 30 real minutes (`utils.h`,
`SECS_PER_MUD_*`).

## The data model

The server keeps a handful of global tables and lists. They are declared in
`db.h`/`comm.h` and defined in `db.c`/`comm.c`.

| Global | Type | Holds |
| --- | --- | --- |
| `descriptor_list` | linked by `next` | every connection |
| `character_list` | linked by `next` | every character in the world: mobiles and playing PCs |
| `object_list` | linked by `next` | every object instance |
| `world[0..top_of_world]` | array | rooms |
| `zone_table[0..top_of_zone_table]` | array | zones and their reset commands |
| `mob_proto[]`, `mob_index[0..top_of_mobt]` | arrays | mobile prototypes and their index |
| `obj_proto[]`, `obj_index[0..top_of_objt]` | arrays | object prototypes and their index |
| `trig_index[0..top_of_trigt-1]` | array of pointers | trigger prototypes |
| `shop_index[0..top_shop]`, `aquest_table[0..total_quests-1]` | arrays | shops and autoquests |
| `player_table[0..top_of_p_table]` | array | the player index (names, ids, levels) |

### Descriptors and characters

A `struct descriptor_data` is a network connection; a `struct char_data` is a
body in the game. The comment in `structs.h` calls them soul and body.

```
descriptor_data ---character--->  char_data  <---desc--- (back pointer)
       |                              |
       |---original---> char_data     (the immortal's own body while switched)
       |---snooping/snoop_by---> other descriptors
       '---olc---> oasis_olc_data     (while in an editor)
```

- An NPC has `desc == NULL` unless an immortal has switched into it. Then
  `ch->desc->original` is the immortal's real body.
- A link-dead player stays in the world with `desc == NULL`.
- During login, `d->character` belongs to the descriptor and is not in
  `character_list`. `interpreter.c:enter_player_game` puts it in the list and
  in a room.
- PCs have their own `player_specials`. Every NPC points at the shared
  `dummy_mob`; never write through it. Accessors built on
  `CHECK_PLAYER_SPECIAL` log a `SYSERR` when used on a mobile (see
  [foundations.md](foundations.md)).

### Objects

An object is in exactly one place at a time:

| Field set | Where it is | List it is on |
| --- | --- | --- |
| `in_room != NOWHERE` | on the floor | `world[room].contents` |
| `carried_by` | in an inventory | `ch->carrying` |
| `worn_by`, `worn_on` | in an equipment slot | `ch->equipment[worn_on]` |
| `in_obj` | inside a container | `container->contains` |

Inventory, room and container lists are linked through `next_content`.
Always move objects with the `handler.c` primitives (`obj_to_room`,
`obj_from_room`, `obj_to_char`, `obj_from_char`, `obj_to_obj`,
`obj_from_obj`, `equip_char`, `unequip_char`). They keep carried weight,
item counts, room light and the `ROOM_HOUSE_CRASH` flag in step. Editing
the pointers by hand breaks those counters. `obj_to_room()` appends to the
room list; `char_to_room()` prepends to `world[room].people`.

### Rooms and zones

`world[]` is an array of `struct room_data`. Each room has its people
(`people`, linked by `next_in_room`), its contents, up to `NUM_OF_DIRS` (10)
exits in `dir_option[]` (only 6 are used unless `CONFIG_DIAGONAL_DIRS` is
set; `DIR_COUNT` gives the live count) and a zone rnum. An exit's `to_room`
is an rnum after boot. `zone_table[]` holds each zone's vnum range, lifespan,
reset mode and its reset command array, terminated by an `'S'` command.

### Prototypes and instances

Mobiles and objects are loaded once as prototypes. `db.c:read_mobile` and
`db.c:read_object` create an instance by copying the whole prototype struct
(`*mob = mob_proto[i]`) and then adjust the copy. This has two consequences
that much of the code depends on:

- **Strings are shared.** An instance's name, descriptions and extra
  descriptions point at the prototype's strings. `db.c:free_char` and
  `db.c:free_obj` (through `genobj.c:free_object_strings_proto`) free a
  string only if it differs from the prototype's pointer. To change one
  instance's text, assign a new `strdup()`ed string; never modify or free the
  shared one.
- **The index counts instances.** `mob_index[rnum].number` and
  `obj_index[rnum].number` count live instances. `read_*` increments them and
  extraction decrements them; zone resets use them for load limits.

Mobile prototypes store hit-point dice in `points.hit`, `points.mana` and
`points.move` (XdY+Z); `read_mobile()` rolls `max_hit` from them. Editing a
prototype in OLC has to update or renumber live instances; see
[olc.md](olc.md).

## Real numbers and virtual numbers

A **vnum** (virtual number) is the number a builder gives a room, mobile,
object, zone, trigger, shop or quest in the world files. An **rnum** (real
number) is an index into the array that holds it now. Both are `IDXTYPE`,
an `unsigned short`, so vnums run from 0 to 65534. The value 65535 is the
"none" sentinel under four names: `NOWHERE`, `NOTHING`, `NOBODY`, `NOFLAG`.

Rules:

- **Tops are inclusive for most arrays.** `top_of_world`, `top_of_mobt`,
  `top_of_objt` and `top_of_zone_table` are the last valid index.
  `top_of_trigt` and `total_quests` are counts, and `top_shop` is the last
  index of `shop_index` (or -1). `VALID_ROOM_RNUM(r)` is
  `r != NOWHERE && r <= top_of_world`.
- **Lookups are binary searches.** `real_room()`, `real_mobile()`,
  `real_object()` and `real_zone()` (`db.c`) search their arrays by vnum, so
  each array must be sorted by vnum. Boot builds them in file order, so the
  index files must list world files in ascending order and each file must
  list records in ascending order. The base branch does not check this;
  branch `audit-deferred-db-loaders` (commit `f71f4cd`) rejects record numbers
  that do not fit a vnum and reports records out of order.
- **Rnums are converted once at boot.** `renum_world()` turns exit
  destinations into rnums; `renum_zone_table()` does the same for zone
  commands and disables (`'*'`) any command that names a missing vnum.
- **Rnums go stale.** Inserting or deleting a room, mobile or object in OLC
  shifts every later rnum and renumbers the references the editors know
  about ([olc.md](olc.md)). A local rnum variable held across such a call,
  or any rnum written to disk, is wrong afterwards. Persistent data always
  stores vnums: load rooms, rent files, configuration start rooms.
- `IN_ROOM(ch)` is an rnum. `GET_ROOM_VNUM(rnum)`, `GET_MOB_VNUM(ch)` and
  `GET_OBJ_VNUM(obj)` return the sentinel when the rnum is invalid.

## Creation, extraction and the purge list

### Creating

`read_mobile()` and `read_object()` instantiate prototypes and prepend the
result to `character_list` or `object_list`. `create_char()` and
`create_obj()` make blank ones. A mobile or object gets its DG UID lazily
(`char_script_id()`, `obj_script_id()`), so `script_id` is 0 until a script
or lookup needs it; see [dg-scripts.md](dg-scripts.md).

### Objects are freed at once

`handler.c:extract_obj` unequips or removes the object from wherever it is,
unseats anyone sitting on it, extracts its contents recursively, unlinks it
from `object_list`, decrements the index count, frees its scripts and
events, and frees the memory. The pointer is dead on return. Any loop over a
contents list must save `next_content` before calling anything that can
extract, and must not trust a saved pointer after running a script (see
`docs/maintainer-safety-guide.md`).

### Characters are freed at the end of the pulse

`handler.c:extract_char` does not free anything. It unseats the character,
cancels its mud events (`clear_char_event_list()`), sets `MOB_NOTDEADYET`
or `PLR_NOTDEADYET` and increments `extractions_pending`. The character
stays in `character_list` and in its room until the last step of the pulse,
when `handler.c:extract_pending_chars` walks the list, calls
`extract_char_final()` for each marked character and unlinks it. This is why
`ch = ch->next` loops survive a death in the middle of the loop: the dying
character is still linked. Calling `extract_char()` twice is harmless.

`DEAD(ch)` tests the two flags. Code that acts on characters (for example
`interpreter.c:special`) should skip characters pending extraction.

`handler.c:extract_char_final`:

1. Exits the whole server with `exit(1)` if the character is in `NOWHERE`.
2. Returns a switched immortal to their own body, in both directions.
3. For a player whose descriptor is still attached: marks every other
   descriptor with the same idnum `CON_CLOSE`, puts this descriptor back at
   the main menu (`CON_MENU`) and keeps the `char_data`.
4. Breaks follower and group links, drops inventory and equipment on the
   floor, stops every fight that involves the character, clears it from
   mobile hunting and memory, and removes it from the room.
5. For a mobile: decrements the index count and frees scripts and script
   memory. For a player: `save_char()` and `Crash_delete_crashfile()`.
6. Frees the character unless it is a player still at the menu.

### Characters and closing connections

`comm.c:close_socket` treats the character according to the descriptor
state. A character that was playing (or in OLC, or in `CON_DISCONNECT`)
stays in the world link-dead and is saved. A character still in the login
or menu states is freed with `free_char()`. See
[networking.md](networking.md#closing-a-connection).

## Zone resets

`db.c:zone_update` runs every `PULSE_ZONE` (10 s). Once per real minute it
ages every zone whose reset mode is not 0. A zone whose age reaches its
lifespan goes on `reset_q` and its age is set to `ZO_DEAD` (999) so it is not
queued twice. Each call then resets at most one queued zone: a zone with
reset mode 2 resets at once, a zone with mode 1 only when no mortal player is
in it (`is_empty()`; an immortal with `PRF_NOHASSLE` does not count).

`db.c:reset_zone` executes the zone's commands in order until `'S'`. Each
command has an `if_flag`: when set, the command runs only if the previous
command ran (`last_cmd`).

| Command | Arguments (after boot) | Action |
| --- | --- | --- |
| `M` | mob rnum, max existing, room rnum | load a mobile if fewer than max exist; becomes the target of `G`/`E` |
| `O` | obj rnum, max existing, room rnum or `NOWHERE` | load an object into a room |
| `G` | obj rnum, max existing | give an object to the last loaded mobile |
| `E` | obj rnum, max existing, wear slot | equip the last loaded mobile; if its wear trigger refuses, the object goes to the inventory |
| `P` | obj rnum, max existing, container obj rnum | put an object in the most recently loaded instance of the container (`get_obj_num()`) |
| `D` | room rnum, direction, state | set this side of a door to open (0), closed (1) or locked (2); the other side needs its own `D` command |
| `R` | room rnum, obj rnum | remove one instance of an object from a room |
| `T` | trigger type, trigger rnum, room rnum | attach a trigger to the last mobile, object or a room |
| `V` | trigger type, context, room rnum, plus name and value strings | set a script variable |
| `*` | | ignored (comments and disabled commands) |

Load triggers run inside the reset (`load_mtrigger`, `load_otrigger`), so a
builder script can purge the object or mobile the reset just loaded. On the
base branch `reset_zone()` keeps using such an object; branch
`worktree-wf_c613c2c5-f9f-41` (commit `28fa9fe`) re-validates it by UID after
each trigger. The reset command format on disk is described in
[persistence.md](persistence.md#zones-zon).

## Event systems

There are two layers. The DG event queue is the scheduler. Mud events are a
typed wrapper on top of it that ties an event to an owner.

### The DG event queue (`dg_event.c`)

There is one global queue, `event_q`, created by `event_init()`. It has
`NUM_EVENT_QUEUES` (10) buckets. An element's key is the absolute pulse at
which it is due; it lives in bucket `key % 10`, and each bucket is kept in key
order.

- `event_create(func, obj, when)` schedules `func(obj)` `when` pulses from
  now (at least 1). The key is `pulse + when`, clamped if that would
  overflow.
- `event_process()` runs first in every heartbeat. For each due event it
  sets `q_el = NULL` (meaning "running"), calls the function, and:
  - re-queues the event if the function returned a positive number of
    pulses;
  - otherwise frees it. For a mud event it also frees the
    `mud_event_data`; for any other event **the callback must already have
    freed its own `event_obj`**.
- `event_cancel(event)` dequeues the event if it is queued, frees its
  `event_obj` (with `free_mud_event()` for mud events, `free()` otherwise)
  and frees the event.
- `event_free_all()` (from `destroy_db()`) frees the whole queue.

Invariant: an event callback must not cancel its own event. On the base
branch, a callback that reaches `event_cancel()` on the running event (for
example `event_whirlwind` -> `hit()` -> death -> `extract_char()` ->
`clear_char_event_list()`) frees the event while `event_process()` still
holds it, and `event_process()` then writes to or frees it again. Branches
`worktree-wf_c613c2c5-f9f-34` (commit `1b47db3`) and
`worktree-wf_c613c2c5-f9f-22` (commit `0eecf95`) defer that free until the
callback returns.

DG `wait` uses this queue directly; see [dg-scripts.md](dg-scripts.md).

### Mud events (`mud_event.c`)

`mud_event_index[]` maps each event id to a name, a callback and an owner
type:

| Id | Callback | Owner | Used by |
| --- | --- | --- | --- |
| `ePROTOCOLS` | `interpreter.c:get_protocols` | descriptor | telnet negotiation, 1.5 s after connect |
| `eWHIRLWIND` | `act.offensive.c:event_whirlwind` | character | the whirlwind skill |
| `eSPL_DARKNESS` | `mud_event.c:event_countdown` | room | the darkness spell |

`NEW_EVENT(id, owner, string, pulses)` calls
`attach_mud_event(new_mud_event(id, owner, string), pulses)`. The
`mud_event_data` holds the owner pointer (`pStruct`), an optional
`strdup()`ed string (`sVariables`) and the event. `attach_mud_event()` also
adds the event to the owner's list: `world_events`, `d->events` (created by
`init_descriptor()`), `ch->events` or `room->events` (both created on first
use).

Owners cancel their events when they go away: `close_socket()`,
`extract_char()` (through `clear_char_event_list()`), `free_char()`,
`extract_obj()` and, for rooms, `destroy_db()`. `free_mud_event()` unlinks
the event from its owner's list, frees the string and the data, and clears
`pEvent->event_obj`.

Known problems on the base branch, all fixed on branch `audit-fix-H3`:

- The owner cancel loops iterate with `simple_list()`, but
  `remove_from_list()` resets the iterator that `simple_list()` shares, so a
  loop cancels only the first event and `free_list()` leaks the remaining
  items (commit `a2efc45`).
- `free_mud_event()` frees `ch->events` or `room->events` when the last
  event goes and sets it to `NULL`; the teardown code then calls
  `free_list()` on that `NULL` pointer. A darkness event pending at shutdown
  or room deletion crashes the server (commit `9c5a87a`).
- A room event stores `&world[rnum]`. OLC reallocates `world[]` when it adds
  or deletes a room, leaving the pointer dangling (commit `b671317` stores
  the room vnum instead).

## Generic lists

`lists.c` provides a doubly linked list of `void *` items
(`create_list`, `add_to_list`, `remove_from_list`, `free_list`,
`random_from_list`, `randomize_list`). Every list is also registered on
`global_lists` for debugging. Groups and mud event owner lists use it.

`simple_list(list)` is the convenient iterator: call it repeatedly until it
returns `NULL`. It keeps its position in file-static state, so:

- it is not reentrant: iterating a second list inside the loop resets the
  first (and logs "simple_list() forced to reset itself");
- leaving a loop early leaves the state set; call `simple_list(NULL)` or
  `clear_simple_list()` before the next loop;
- on the base branch, removing an item inside the loop ends the loop (see
  the mud event problems above).

For nested iteration use an explicit `struct iterator_data` with
`merge_iterator()`, `next_in_list()` and `remove_iterator()`.

## Where each subsystem lives

| Subsystem | Files | Document |
| --- | --- | --- |
| Process, sockets, output buffers, `act()` | `comm.c`, `comm.h` | [networking.md](networking.md) |
| Telnet options, MSDP/MSSP/MXP, colour | `protocol.c`, `protocol.h`, `telnet.h` | [networking.md](networking.md) |
| Login, command table, argument parsing, aliases | `interpreter.c`, `interpreter.h` | [networking.md](networking.md) |
| Site bans, name filter | `ban.c`, `ban.h` | [networking.md](networking.md) |
| String editor and pager | `modify.c`, `improved-edit.c` | [networking.md](networking.md) |
| World loading, zone reset, instantiation, teardown | `db.c`, `db.h` | [persistence.md](persistence.md), this document |
| Player files, rent, houses, mail, boards, IBT | `players.c`, `objsave.c`, `house.c`, `mail.c`, `boards.c`, `ibt.c` | [persistence.md](persistence.md) |
| Configuration | `config.c`, `cedit.c`, `db.c:load_config` | [persistence.md](persistence.md) |
| World primitives (move, equip, extract, find) | `handler.c`, `handler.h` | this document, [gameplay.md](gameplay.md) |
| Events and lists | `dg_event.c`, `mud_event.c`, `lists.c` | this document |
| Core types, macros, diagnostics, portability | `structs.h`, `utils.h`, `utils.c`, `sysdep.h`, `zmalloc.c`, `random.c`, `constants.c` | [foundations.md](foundations.md) |
| Triggers | `dg_*.c` | [dg-scripts.md](dg-scripts.md) |
| Online creation | `oasis*.c`, `*edit.c`, `gen*.c` | [olc.md](olc.md) |
| Commands, combat, magic, mobiles, shops, quests | `act.*.c`, `fight.c`, `magic.c`, `spell*.c`, `mobact.c`, `shop.c`, `quest.c`, `class.c`, `limits.c` | [gameplay.md](gameplay.md) |
