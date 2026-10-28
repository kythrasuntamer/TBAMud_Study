# Gameplay: commands, combat, magic and the living world

This document explains the code that makes the game play: how a typed
command reaches its handler, how characters and objects move between rooms,
inventories, equipment and containers, how they are destroyed, how combat
and magic work, and the periodic machinery (mobile AI, regeneration, timers,
weather, events) that keeps the world moving between commands. It also
covers shops, quests, special procedures, pathfinding, the ASCII map and the
class tables.

It is written for maintainers who will change this code. The most important
parts are the rules about **lifetime** (what is freed when, and which
pointers survive a callback) and **membership** (which lists an entity is
on, and which functions keep those lists consistent). Everything here
describes the code at the head of this branch; fixes that exist only on the
audit fix branches are listed separately in
[Pending changes on audit fix branches](#pending-changes-on-audit-fix-branches).

## Source files

| File | Role |
| --- | --- |
| `src/interpreter.c` (part) | `command_interpreter()`, `special()`: from a typed line to a command handler. |
| `src/act.comm.c` | Speech and channels: `say`, `tell`, `whisper`, `gsay`, `page`, the gossip-style channels, `write`. |
| `src/act.informative.c` | Looking and information: `look`, `examine`, `score`, `who`, `where`, `help`, `consider`, `scan`, toggles. |
| `src/act.item.c` | Object handling: `get`, `put`, `drop`, `give`, `wear`, `remove`, `eat`, `drink`, `pour`, `sacrifice`. |
| `src/act.movement.c` | Moving through exits (`do_simple_move()`, `perform_move()`), doors and containers, positions and furniture, following. |
| `src/act.offensive.c` | Starting and shaping fights: `hit`, `kill`, `backstab`, `bash`, `kick`, `rescue`, `flee`, `order`, `whirlwind`, `bandage`. |
| `src/act.other.c` | Everything else a player does: `quit`, `save`, `steal`, `practice`, groups, `split`, `use`/`quaff`/`recite`, preference toggles, endgame points. |
| `src/act.social.c` | Socials (`do_action()`), `gmote`, and merging socials into the command table. |
| `src/act.wizard.c` | Immortal and builder commands. |
| `src/handler.c`, `src/handler.h` | The world-manipulation primitives: moving characters and objects, equipment, affects, extraction, visibility-aware lookups, money objects, groups. |
| `src/fight.c`, `src/fight.h` | Combat: the combat list, the violence round, to-hit, damage, death, corpses, experience. |
| `src/spell_parser.c` | The spell table (`spell_info[]`), `do_cast`, `cast_spell()`, `call_magic()`, magic items. |
| `src/magic.c` | Spell templates (`mag_damage()`, `mag_affects()`, ...), saving throws, `affect_update()`. |
| `src/spells.c`, `src/spells.h` | Manual spells (recall, teleport, summon, charm, identify, ...); spell and skill numbers. |
| `src/mobact.c` | Mobile AI per `PULSE_MOBILE`; mobile memory. |
| `src/limits.c` | The hourly tick (`point_update()`), regeneration, experience and levelling, hunger and thirst, idling, coin helpers. |
| `src/shop.c`, `src/shop.h` | The shopkeeper special procedure and the shop runtime. |
| `src/quest.c`, `src/quest.h` | The autoquest runtime: questmasters, quest commands, completion. |
| `src/spec_procs.c`, `src/spec_assign.c`, `src/castle.c` | Special procedures and their assignment at boot. |
| `src/mud_event.c`, `src/mud_event.h` | Typed "mud events" layered on the DG event queue. |
| `src/lists.c`, `src/lists.h` | A generic doubly linked list with iterators (groups, event lists). |
| `src/graph.c` | Breadth-first pathfinding (`track`, mobile hunting). |
| `src/weather.c` | Game time and the weather model. |
| `src/asciimap.c` | The `map` command and the automap beside room descriptions. |
| `src/class.c`, `src/class.h` | Class tables: saving throws, THAC0, experience per level, titles, practice parameters, level-up gains. |

The world-file loaders, zone resets, player saving and the game loop itself
belong to [persistence.md](persistence.md) and [overview.md](overview.md).
The DG script engine, which most gameplay code calls into, is in
[dg-scripts.md](dg-scripts.md). OLC is in [olc.md](olc.md).

## The pulse: what runs when

The server runs in pulses of `OPT_USEC` microseconds (100 ms, so
`PASSES_PER_SEC` is 10). In each pass of the game loop, input is read and
each descriptor runs at most one command, output is flushed, and then
`comm.c:heartbeat()` runs once for every pulse that has elapsed (several if
the server lagged, capped at 30 seconds' worth). The loop itself is
described in [overview.md](overview.md). This is what `heartbeat()` calls:

| Interval | Call | What it drives |
| --- | --- | --- |
| every pulse, first | `dg_event.c:event_process()` | due DG `wait`s and mud events ([dg-scripts.md](dg-scripts.md#the-event-queue), [Events and generic lists](#events-and-generic-lists)) |
| 13 s (`PULSE_DG_SCRIPT`) | `dg_scripts.c:script_trigger_check()` | random triggers |
| 1 s | `msdp_update()`, `next_tick--` | protocol variables; the tick countdown shown to players |
| 10 s (`PULSE_ZONE`) | `db.c:zone_update()` | zone resets ([persistence.md](persistence.md)) |
| 15 s (`PULSE_IDLEPWD`) | `check_idle_passwords()` | login timeouts ([networking.md](networking.md)) |
| 10 s (`PULSE_MOBILE`) | `mobact.c:mobile_activity()` | [Mobile AI](#mobile-ai) |
| 2 s (`PULSE_VIOLENCE`) | `fight.c:perform_violence()` | one [combat](#combat) round |
| 75 s (one mud hour, `SECS_PER_MUD_HOUR`) | `weather_and_time(1)`, `check_time_triggers()`, `magic.c:affect_update()`, `limits.c:point_update()`, `quest.c:check_timed_quests()` | the **tick**: time and weather, time triggers, affect durations, regeneration and decay, quest timers |
| `CONFIG_AUTOSAVE_TIME` minutes | `Crash_save_all()`, `House_save_all()` | autosave |
| 5 min / 30 min | `record_usage()` / `save_mud_time()` | statistics, the game clock file |
| every pulse, last | `handler.c:extract_pending_chars()` | frees characters extracted during this pulse ([Character and object lifetime](#character-and-object-lifetime)) |

Two orderings matter. Events run before anything else in the pulse, and
character extraction runs after everything else, so a character marked for
extraction by anything in the pulse (a command typed just before it, a
combat round, a script) stays allocated until the end of that pulse.

Durations measured in **ticks** (affects, object timers, hunger, quest time
limits, `wait <n> t` in scripts) are mud hours of 75 real seconds.
Combat-related waits (`WAIT_STATE()`) are measured in pulses.

## How a command runs

```
comm.c game loop        GET_WAIT_STATE(ch) > 0 ?  -> skip this descriptor (decrement)
  get_from_q()          one line per descriptor per pass
  pager / string editor / nanny()   (not playing)
  perform_alias()
  command_interpreter(ch, line)
     command_wtrigger / command_mtrigger / command_otrigger   -> a script may take the command
     look up the word in complete_cmd_info[] (commands, then socials)
     checks: level, frozen, NPC using an immortal command, position
     special(ch, cmd, arg)                                     -> a spec proc may take the command
     (*command_pointer)(ch, arg, cmd, subcmd)                  -> the ACMD
```

`interpreter.c:command_interpreter()`:

1. Clears `AFF_HIDE` (any command reveals a hidden character).
2. Splits off the first word. A line starting with a non-letter takes that
   single character as the command (`'hello` is `say hello`).
3. Runs the **command triggers** of the room, then of the mobiles in the
   room, then of the objects the actor has and sees, with the raw word and
   the rest of the line. If any returns non-zero the command is finished.
   Command triggers therefore see every word, including words that are not
   commands.
4. Lets an implementor switched into a mobile use the mobile script commands
   (`mecho`, `mload`, ...) through `script_command_interpreter()`.
5. Looks the word up in `complete_cmd_info[]` by prefix, **in table order**,
   so the order of `interpreter.c:cmd_info[]` decides what an abbreviation
   means. Ordinary commands are searched first and socials second. Commands
   above the character's level are skipped. Not found: "Huh?!?" plus
   "Did you mean" suggestions within edit distance 2.
6. Refuses frozen players, unimplemented commands, immortal commands typed
   by any NPC (a switched immortal, or a mobile running a script or an
   order), and commands whose `minimum_position` is above the character's
   position (with a message per position).
7. Offers the command to `special()` and, if no special procedure takes it,
   calls the handler.

A handler is declared with `ACMD(name)`:
`void name(struct char_data *ch, char *argument, int cmd, int subcmd)`.
`cmd` is the index into `complete_cmd_info[]` (spec procs compare it with
`CMD_IS("buy")`); `subcmd` lets one handler serve several commands
(`do_gen_door` for open/close/lock/unlock/pick, `do_move` for the
directions). `argument` points into the game loop's line buffer; it is
writable and valid only during the call. Several handlers modify it in place
(`do_cast` tokenizes it with `strtok()`).

The game loop decrements `GET_WAIT_STATE(ch)` once per pulse and runs no
command for a character whose wait is still positive. `WAIT_STATE(ch, n)`
sets it after skills and spells. NPCs use the same field
(`GET_MOB_WAIT()`), but only `perform_violence()` counts it down, by
`PULSE_VIOLENCE` per round.

Other code runs commands for characters by calling `command_interpreter()`
directly: `order`, DG `force`, mobile scripts, `at`. Every such call is a
full callback: triggers and special procedures run inside it.

### Invariants

- A command handler may run any number of DG scripts and special
  procedures. Treat every call to `command_interpreter()` as a callback
  that can move, kill or extract anyone.
- `argument` belongs to the caller and is valid only during the call.
- The order of `cmd_info[]` is part of the user interface (abbreviations)
  and of spec procs (`do_simple_move()` passes `dir + 1` as the command
  number, which assumes the direction commands follow the reserved entry 0).

## Command families

| File | Commands | Notes for maintainers |
| --- | --- | --- |
| `act.comm.c` | `say`, `gsay`, `tell`/`reply`, `whisper`/`ask`, `write`, `page`, `holler`/`shout`/`gossip`/`auction`/`grats` (`do_gen_comm`), `qsay`/`qecho` | Player text is embedded in `act()` formats; the rendered result is stored in each listener's channel history (`add_history()`). `do_say` sends its messages with `DG_NO_TRIG` and then fires speech triggers. |
| `act.informative.c` | `look`, `examine`, `read`, `exits`, `score`, `inventory`, `equipment`, `time`, `weather`, `help`, `who`, `users`, `where`, `levels`, `consider`, `diagnose`, `toggle`, `commands`, `history`, `whois`, `areas`, `scan` | Formats world, character and object state; depends on table indices from world files being valid. `look_at_room()` adds the automap ([The ASCII map](#the-ascii-map)). |
| `act.item.c` | `get`, `put`, `drop`/`junk`/`donate`, `give`, `drink`, `eat`/`taste`, `pour`/`fill`, `wear`, `wield`, `grab`, `remove`, `sacrifice` | Runs object and room triggers between the checks and the action; see [Trigger call sites](#trigger-call-sites-in-gameplay-code). Keeps drink-container names and container weights consistent. |
| `act.movement.c` | the directions (`do_move`), `open`/`close`/`lock`/`unlock`/`pick` (`do_gen_door`), `enter`, `leave`, `stand`, `sit`, `rest`, `sleep`, `wake`, `follow`, `unfollow` | [Movement](#movement) below. |
| `act.offensive.c` | `assist`, `hit`, `kill`, `backstab`, `order`, `flee`, `bash`, `rescue`, `kick`, `whirlwind`, `bandage` | Starts fights through `hit()`/`damage()`. `kill` by a `LVL_GRGOD`+ with no-hassle calls `raw_kill()` directly. |
| `act.other.c` | `quit`, `save`, `sneak`, `hide`, `steal`, `practice`, `visible`, `title`, `group` and its subcommands, `report`, `split`, `use`/`quaff`/`recite`, `display`, the preference toggles (`do_gen_tog`), `happyhour`, `endgame` | `quit` saves and extracts. `use`/`quaff`/`recite` go to `mag_objectmagic()`. `steal` runs give and receive triggers. |
| `act.social.c` | every social (`do_action`), `gmote` | Any field of a social may be NULL (`#` in the socials file), meaning "no message". |
| `act.wizard.c` | 45 immortal commands | See below. |

`act.wizard.c` groups by purpose:

| Purpose | Commands |
| --- | --- |
| Moving | `goto`, `trans`, `teleport` (these fire room enter triggers), `at` (does not) |
| Inspecting | `stat`, `vstat`, `show`, `vnum`, `where` (immortal form, in `act.informative.c`), `last`, `plist`, `recent`, `links`, `checkload`, `date`, `file`, `changelog` |
| Changing the world | `load` (fires load triggers), `purge`, `zreset`, `zpurge`, `zlock`/`zunlock`, `oset` |
| Changing characters | `set`, `advance`, `restore`, `invis`, `wizutil` (freeze, thaw, mute, ...), `force`, `peace` |
| Controlling sessions | `snoop`, `switch`, `return`, `dc` |
| Communicating | `echo`, `send`, `gecho`, `wiznet` |
| Running the server | `shutdown`, `copyover`, `wizlock`, `saveall`, `wizupdate` |

`stat` on offline players and on prototypes builds temporary structures and
frees them again; `copyover` re-executes the server
([networking.md](networking.md)). `do_at` moves the immortal to the target,
runs the command through `command_interpreter()` and moves back only if the
immortal is still in the target room; it holds the original room as an
**rnum** across the command, so a command that inserts a room (`dig`)
can send the immortal back to the wrong room.

### Movement

`act.movement.c:perform_move(ch, dir, need_specials_check)` checks that the
exit exists (or that `buildwalk` creates it), is not closed (immortals with
no-hassle pass), and moves the character with `do_simple_move()`. If the
move succeeds it moves each follower that was in the old room and standing,
recursively, with `need_specials_check` set.

`do_simple_move()` runs the following in order and stops at the first
refusal:

1. For a follower only, `special(ch, dir + 1, "")`. (The leader's
   spec-proc check already happened in `command_interpreter()`.)
2. Leave triggers: mobile (`leave_mtrigger`), room (`leave_wtrigger`),
   objects in the room (`leave_otrigger`). After each one the move is
   abandoned if a trigger refused **or moved the character**
   (`IN_ROOM(ch) != was_in`).
3. Charm, boat, flight, water-breathing, house, closed-zone, no-immortal
   zone, tunnel capacity and god-room checks; movement points (average of
   the two sectors' `movement_loss[]`; NPCs and immortals do not pay).
4. The leave message, `char_from_room()`, `char_to_room()`.
5. Entry triggers: the moving mobile's own (`entry_mtrigger`) and the new
   room's (`enter_wtrigger`). A refusal moves the character back silently.
6. Arrival message, `look_at_room()`.
7. A death trap (`ROOM_DEATH`, mortals only): `death_cry()`,
   `extract_char()`, and the move "fails" (followers do not follow).
8. Memory and greet triggers of the mobiles in the new room
   (`entry_memory_mtrigger`, `greet_mtrigger`, `greet_memory_mtrigger`).
   A greet refusal moves the character back and shows the old room.

A return of 0 therefore does not mean "nothing happened": the character
may have been moved by a trigger, sent back, or marked for extraction.

**Follower loop hazard.** `perform_move()` saves `k->next` before moving
follower `k`, and the follower's move runs triggers. `stop_follower()` frees
`follow_type` nodes, so a script that makes the *next* follower stop
following leaves the loop holding a freed node. `act.offensive.c:do_order`
is worse: `order followers <command>` reads `k->next` *after* running the
command for `k->follower`, so anything in that command that reaches
`stop_follower()` for that follower frees `k` before it is read.

**Doors.** `do_gen_door()` finds the door or container and `do_doorcmd()`
fires `door_mtrigger` and `door_wtrigger` before acting. Neither result is
re-checked against the world: a door script that removes the exit
(`wdoor ... purge`) leaves `do_doorcmd()` dereferencing a NULL exit.

**Furniture.** A sitting character has `SITTING(ch)` (the furniture) and
`NEXT_SITTING(ch)` (the next sitter); the furniture has `OBJ_SAT_IN_BY()`
(the first sitter) and an occupant count in value 1.
`utils.c:char_from_furniture()` keeps the three consistent and is called by
`char_from_room()` and `extract_char()`. In this branch
`handler.c:obj_from_room()` clears each sitter's pointers but leaves the
furniture's list head and count, and `set_fighting()`/`stop_fighting()`
change position without unseating, so the furniture can keep a pointer to a
character that is later freed.

### Trigger call sites in gameplay code

Gameplay code calls DG dispatchers between deciding to do something and
doing it. Most dispatchers return 0 to veto the action. What happens inside
them is described in [dg-scripts.md](dg-scripts.md#trigger-dispatch); this
table is about what the **caller** relies on afterwards.

| Caller | Triggers | What the caller assumes afterwards |
| --- | --- | --- |
| `interpreter.c:command_interpreter()` | command (room, mob, obj) | non-zero: the command is done; nothing else is touched. |
| `comm.c:perform_act()` (every `act()`) | act triggers of each NPC recipient, unless `DG_NO_TRIG` | `act()` keeps walking `next_in_room` of the recipient; the caller keeps using its own pointers. |
| `act.comm.c:do_say()` | speech (mob, room) | nothing after. |
| `act.movement.c:do_simple_move()` | leave, entry/enter, greet, memory | re-checks the room after leave triggers; see [Movement](#movement). |
| `act.movement.c:do_doorcmd()` | door (mob, room) | that the exit or container still exists (it may not). |
| `act.item.c` get, drop, give, put, wear, remove, eat, drink | `get_otrigger`, `drop_otrigger`, `drop_wtrigger`, `give_otrigger`, `receive_mtrigger`, `bribe_mtrigger`, `wear_otrigger`, `remove_otrigger`, `consume_otrigger` | the dispatcher returns 0 if the object no longer matches its UID or left the actor, so the single object is safe; container pointers and the "all"/count loops' saved next object are **not** re-checked. |
| `act.other.c:do_steal()` | `give_otrigger`, `receive_mtrigger` | as above. |
| `fight.c:hit()` | `fight_mtrigger` (attacker), `hitprcnt_mtrigger` (victim) | re-checks that both are in the same room; the wielded weapon was read **before** the trigger. |
| `fight.c:damage()` | `damage_mtrigger` | its return **is** the damage (see [Combat](#combat)). |
| `fight.c:raw_kill()` | `death_mtrigger` | 0 suppresses the death cry; the corpse is made regardless. |
| `fight.c:make_corpse()` | `remove_otrigger` per worn item | ignores the result. |
| `spell_parser.c:call_magic()` | cast (room, obj, mob) | 0 cancels the spell; the targets are used afterwards without re-checking. |
| `spell_parser.c:mag_objectmagic()` | `consume_otrigger` (potions) | the item still exists after its spells ran (it may not). |
| `magic.c:mag_summons()`, `mag_creations()`, `act.wizard.c:do_load()`, `db.c:reset_zone()` | load (mob, obj) | the corpse used by animate dead still exists (it may not). |
| `spells.c` recall, teleport, summon | memory and greet triggers | nothing; refusals are ignored. |
| `limits.c:point_update()` | `timer_otrigger` | its saved next object still exists (it may not). |
| `interpreter.c:enter_player_game()` (login trigger) and `nanny()` (greet and memory triggers) when a player enters the game | login, greet, memory | nothing. |

The caller-side gaps marked "it may not" are known and listed with their
fixes in [Pending changes](#pending-changes-on-audit-fix-branches). The
general rule, and the helpers to follow it, are in
[dg-scripts.md](dg-scripts.md#callback-safety).

### Invariants

- `act()` is a callback point for NPC recipients with act triggers. Pass
  `DG_NO_TRIG` when the caller cannot tolerate scripts running.
- Never iterate an object list (room contents, inventory, container) while
  calling anything that can run a trigger, unless each next element is
  re-acquired afterwards (by UID or from the list head).
- After a veto-style trigger returns 1, re-validate everything the action
  will touch, not only the object the trigger was about.

## Character and object lifetime

### Where an entity can be

A **character** (`struct char_data`) in the game is on `character_list`
from creation until it is freed. It is normally in exactly one room:
`IN_ROOM(ch)` is a valid rnum and the character is on that room's `people`
list (linked through `next_in_room`). `IN_ROOM(ch) == NOWHERE` happens only
transiently inside a move, for characters being created, and for players at
the menu. A character may additionally be on `combat_list` (while
`FIGHTING(ch)` is set), on a master's `followers` list, in a group's member
list, and on a piece of furniture.

An **object** (`struct obj_data`) is on `object_list` from `create_obj()`
until `extract_obj()`. At any time it is in **at most one** of four places:

| Place | Fields | List |
| --- | --- | --- |
| a room | `IN_ROOM(obj)` is a valid rnum | `world[rnum].contents` via `next_content` |
| a character's inventory | `carried_by` | `ch->carrying` via `next_content` |
| a character's equipment | `worn_by`, `worn_on` | `GET_EQ(ch, worn_on)` (an array slot, not a list) |
| a container | `in_obj` | `container->contains` via `next_content` |

An object that is in none of them is "in limbo": fresh from
`read_object()`/`create_obj()`, or between a `from` and a `to` call. Code
must put it somewhere or extract it before control returns to the game
loop.

### The movement primitives

All placement goes through `handler.c`. Each `from` function unlinks and
clears the location; each `to` function links and sets it. They do not
check the other three places, so the caller must take an object *out* of
its old place before putting it *in* a new one.

| Function | Preconditions | Side effects |
| --- | --- | --- |
| `char_to_room(ch, room)` | `ch` not in a room | Pushes `ch` on the room's `people`. Runs the room-find and mob-find quest checks. Adds the room light of a lit `WEAR_LIGHT`. Stops the fight if the opponent is elsewhere. An invalid `room` is logged and **ignored**, leaving `ch` nowhere. |
| `char_from_room(ch)` | `ch` in a room | Stops `ch`'s fight, unseats it, removes its light, unlinks it. A NULL `ch` or `NOWHERE` **exits the server** (`exit(1)`). |
| `obj_to_char(obj, ch)` | `obj` placed nowhere | Pushes on `ch->carrying`, adds weight and count, runs the object-find quest check, sets `PLR_CRASH` for players. Sets `IN_ROOM(obj) = NOWHERE`. |
| `obj_from_char(obj)` | `obj->carried_by` set | Unlinks, subtracts weight and count, sets `PLR_CRASH`. |
| `obj_to_room(obj, room)` | `obj` placed nowhere | **Appends** to the room's contents (the list is walked to its end). Marks house rooms for saving. An invalid `room` is logged and ignored. |
| `obj_from_room(obj)` | `IN_ROOM(obj)` valid | Unseats every sitter, unlinks, marks house rooms for saving. |
| `obj_to_obj(obj, cont)` | `obj` placed nowhere, `obj != cont` | Pushes on `cont->contains`. If the container's capacity (value 0) is positive, adds the weight to every enclosing container and to the outermost carrier's `IS_CARRYING_W()`. |
| `obj_from_obj(obj)` | `obj->in_obj` set | The reverse of `obj_to_obj()`. |
| `equip_char(ch, obj, pos)` | slot empty, `obj` neither carried nor in a room | Sets the slot, `worn_by`, `worn_on`; armour AC; room light; adds the object's applies and affect bits; `affect_total()`. An object the character's alignment or class may not use "zaps" and is **put in the inventory** instead. |
| `unequip_char(ch, pos)` | slot occupied | The reverse, and **returns the object placed nowhere**: the caller must put it somewhere. |

`handler.c:object_list_new_owner()` rewrites `carried_by` for a whole tree
(used when an inventory list is moved wholesale).

### Affects

A character's `real_abils` are its base abilities; `aff_abils` and the
point maxima (`GET_MAX_HIT()`, AC, hitroll, damroll, saves) hold the
*affected* values. `handler.c:affect_total()` recomputes them: it
subtracts every equipment apply and every affect, resets `aff_abils` to
`real_abils`, adds everything back, and clamps abilities to 0..18 for
mortals (0..25 for NPCs and greater gods; strength above 18 becomes
`GET_ADD()` for mortals).

| Function | Ownership |
| --- | --- |
| `affect_to_char(ch, af)` | Copies `*af` into a new node at the head of `ch->affected`; the caller keeps `af` (usually a stack variable). |
| `affect_remove(ch, af)` | `af` must be on `ch->affected`; unlinks and **frees** it. |
| `affect_from_char(ch, spell)` | Removes every affect of that spell. |
| `affect_join(ch, af, add_dur, avg_dur, add_mod, avg_mod)` | Merges with the first affect of the same spell and location (adding or averaging duration and modifier), or adds it. The sums are stored in an `sbyte` modifier and `sh_int` duration without overflow checks. |

Durations count ticks and are decremented by `magic.c:affect_update()`; a
duration of -1 is permanent. When an affect expires its spell's
`wear_off_msg` is shown, unless the next affect in the list belongs to the
same spell and has not expired (multi-part spells print once).

### Extraction

The two kinds of entity are destroyed differently, and almost every lifetime
rule in the code base follows from the difference.

**Characters are extracted late.** `handler.c:extract_char(ch)` only:

1. unseats `ch` from furniture,
2. cancels its mud events (`clear_char_event_list()`), and
3. sets `MOB_NOTDEADYET` (NPCs) or `PLR_NOTDEADYET` (players) and counts
   one pending extraction (a second call on the same character is
   ignored).

The character stays on `character_list`, in its room and in every other
list. At the end of the pulse `extract_pending_chars()` walks
`character_list`, calls `extract_char_final()` for every flagged character
and unlinks it from the list. `extract_char_final()`:

1. requires a room (`NOWHERE` **exits the server**);
2. returns a switched immortal to its own body, or sends a player with a
   descriptor back to the menu (`CON_MENU`) and closes any other descriptor
   logged in as the same player;
3. releases followers (`die_follower()`) and leaves the group;
4. drops inventory and equipment into the room;
5. stops every fight involving `ch`, clears `HUNTING()` of every NPC
   hunting it, and makes NPCs forget a player who died;
6. removes `ch` from its room;
7. for NPCs, decrements the prototype's live count, frees its memory list,
   extracts its script and DG memory; for players, saves the character and
   deletes the crash file;
8. frees the struct with `db.c:free_char()` unless it is a player still
   attached to a descriptor (who is now at the menu).

`free_char()` frees the strings the character does not share with its
prototype, its affects, script, events and followers, detaches the
descriptor and removes the character's UID from the DG lookup table
(ownership-checked; see
[dg-scripts.md](dg-scripts.md#unique-ids-and-the-lookup-table)).

Why so late? Because code everywhere walks `character_list` and room
`people` lists while calling things that can kill. A character pointer
obtained earlier in the pulse is always safe to *dereference* until the
pulse ends. It may, however, be pending extraction or have moved, so code
that continues after a callback must test `MOB_NOTDEADYET`/`PLR_NOTDEADYET`
(the DG helper is `dg_memory_character_live()`) and the room.

**Objects are extracted at once.** `handler.c:extract_obj(obj)` unequips or
removes the object from wherever it is, unseats sitters, **recursively
extracts its contents**, unlinks it from `object_list`, decrements the
prototype's live count, extracts its script, cancels its events, frees its
private trigger list, and frees it (`db.c:free_obj()`, which also removes
its UID from the lookup table). Every pointer to the object or to anything
inside it is dangling the moment it returns.

Because a DG script can extract any object, code that may run a script
between obtaining an object pointer and using it must re-check the object
by UID without dereferencing it (`dg_scripts.c:obj_uid_matches()`), as the
trigger dispatchers do. Rooms are never freed during play, but `world[]`
can be reallocated by OLC (see
[olc.md](olc.md#renumbering-on-insert-and-delete)).

| Stale pointer to ... | Freed | Safe test |
| --- | --- | --- |
| a character | end of the pulse | `MOB_NOTDEADYET` / `PLR_NOTDEADYET`, `IN_ROOM()` |
| an object | immediately | `obj_uid_matches(uid, obj)` with a UID taken earlier |
| a `follow_type` node | immediately (`stop_follower()`) | re-walk `ch->followers` |
| an affect | immediately (`affect_remove()`) | re-walk `ch->affected` |
| a room | never, but `world[]` may move | keep an rnum or vnum, not `&world[n]` |

### Finding things by name

| Function | Searches |
| --- | --- |
| `get_char_room_vis(ch, name, number)` | the actor's room; `self`/`me` is the actor; `0.name` means a player |
| `get_char_world_vis()` | the room first, then `character_list` |
| `get_player_vis()` | players only, exact name |
| `get_obj_in_list_vis(ch, name, number, list)` | one object list |
| `get_obj_vis()` | inventory, room, then `object_list` |
| `get_obj_in_equip_vis()`, `get_obj_pos_in_equip_vis()` | equipment slots |
| `generic_find(arg, FIND_* bits, ch, &tch, &tobj)` | any combination; returns the bit that matched |

Names may carry a count, `2.sword`: `get_number()` splits it off **in
place** (it rewrites the caller's string) and returns 0 for a malformed
count. `find_all_dots()` recognises `all` and `all.name` and also rewrites
its argument. `isname()` matches any whole word of a keyword list by
abbreviation (numbers must match exactly); `is_name()` is the older
exact-word matcher kept for OLC. Visibility (`CAN_SEE`, `CAN_SEE_OBJ`) is
applied by every `_vis` function.

`fname()` returns the first word of a keyword list in a static buffer that
the next call overwrites.

### Money and groups

`create_money(amount)` makes an `ITEM_MONEY` object with no prototype
(`item_number` is `NOTHING`), keywords `coin(s) gold`, an extra description
describing the pile, and the amount in value 0. Coins picked up are turned
back into gold by `act.item.c:get_check_money()`.

A group (`struct group_data`) has a leader, flags and a member list from
`lists.c`; all groups are on `group_list`. `create_group()` makes one and
joins the leader; `join_group()` adds a member; `leave_group()` removes one,
hands leadership to a random remaining member, and frees the group with
`free_group()` when it becomes empty. `free_group()` must only be called on
an **empty** group: called with members it would free the group from inside
its own member loop.

### Invariants

- An object is in at most one place; take it out (`obj_from_*`,
  `unequip_char()`) before putting it in (`obj_to_*`, `equip_char()`).
- Never return to the game loop with an object in limbo.
- Weight, item counts, room light and affects are maintained only by the
  primitives above. Changing an object's weight, applies or type while it is
  carried, worn or contained corrupts the totals unless it is taken out
  first and put back afterwards.
- A character pointer is dereferenceable until the end of the pulse; an
  object pointer only until anything can call `extract_obj()`.
- `extract_char()` is idempotent; `extract_obj()` is not (a second call is a
  double free).

## Combat

### The combat list

Fighting characters are on `combat_list`, linked through `next_fighting`.
`fight.c:set_fighting(ch, vict)` pushes `ch` on the list, wakes it from
magical sleep, sets `FIGHTING(ch)` and `POS_FIGHTING`; it refuses
player-killing when `pk_allowed()` says no, and calling it for a character
that is already fighting calls `core_dump()` and returns.
`stop_fighting(ch)` unlinks `ch`, clears `FIGHTING(ch)`, stands it up and
re-derives the position from its hit points (`update_pos()`).

`perform_violence()` walks the list with a file-static cursor,
`next_combat_list`. `stop_fighting()` advances that cursor when it removes
the character the loop will visit next. This is what makes it safe for a
round to end other fights. New fights start at the head of the list and are
first visited in the next round.

### One round

For each character on `combat_list`, `perform_violence()`:

1. ends the fight if the opponent is gone or in another room;
2. for NPCs, spends one round of `GET_MOB_WAIT()` if any is left, otherwise
   stands a fallen NPC back up;
3. refuses to fight from below `POS_FIGHTING`;
4. lets each group member with `PRF_AUTOASSIST` (and every NPC member) in
   the room who is standing, not fighting and can see the fighter
   `assist` (`do_assist()`);
5. calls `hit(ch, FIGHTING(ch), TYPE_UNDEFINED)`;
6. calls the mobile's special procedure with `cmd` 0, if it has `MOB_SPEC`
   and is not pending extraction.

### `hit()`

`hit(ch, victim, type)` fires the attacker's fight trigger, gives up if the
two are no longer in one room, and rolls a d20 against
`compute_thaco()` (class THAC0 from `class.c:thaco()` for players, 20 for
NPCs, less the strength, hitroll, intelligence and wisdom bonuses) and the victim's AC (`compute_armor_class()`,
dexterity included when awake, never below -100). A natural 20 or a victim
that is not awake always hits; a natural 1 always misses.

Damage is the strength bonus plus damroll plus the weapon's dice (or the
NPC's bare-hand dice, or 0..2 for a player), multiplied for a victim below
`POS_FIGHTING` by `1 + (POS_FIGHTING - position) / 3`, at least 1, and
multiplied by `backstab_mult()` for a backstab. The comment above that
line promises x1.33 for sitting and x1.66 for resting, but the division is
integer division: the real factors are x1 sitting or resting, x2 sleeping,
stunned or incapacitated, and x3 mortally wounded. `hit()` then calls
`damage()` and finally the victim's hit-percent trigger. The weapon type
(`GET_OBJ_VAL(weapon, 3) + TYPE_HIT`) only selects messages.

### `damage()`

`damage(ch, victim, dam, attacktype)` returns the damage done, 0 for none, or
-1 if the victim died. In order it:

1. returns -1 for a victim already dead and pending extraction (normal with
   delayed extraction); a dead victim *not* pending extraction is logged
   and killed;
2. refuses player-killing (`pk_allowed()`), peaceful rooms (except for the DG
   proxy caster mob), protected shopkeepers and `MOB_NOKILL`;
3. zeroes damage to a no-hassle immortal;
4. lets the victim's damage trigger **replace** the damage (a return of -1
   cancels the hit; a trigger that does not return a number makes it 1);
5. starts the fight in both directions (a `MOB_MEMORY` victim remembers a
   player attacker), makes a pet that is attacked by its master stop
   following, and makes an invisible or hidden attacker appear;
6. halves damage against sanctuary, **caps it at 100**, subtracts it and
   gives the attacker `victim level x damage` experience;
7. prints the messages (`skill_message()` from `lib/misc/messages`, or the
   generic `dam_message()`), and the position messages;
8. makes wimpy NPCs and players under their wimpy level flee, and rescues a
   linkless player by fleeing or by moving them to room rnum 0;
9. stops a stunned or worse victim fighting;
10. on death: experience (`group_gain()` shared between group members in the
    room, or `solo_gain()`), a log line for player deaths, and `die()`;
    then the killer's automatic actions: `autosplit`, `autogold`,
    `autoloot` (`do_get(ch, "all corpse")`) and `autosac`.

Those automatic actions are ordinary commands run after the death, so they
can run get and drop triggers; they act on the first visible object named
"corpse" in the room, which need not be the corpse just made.

### Death

`die(ch, killer)` takes half the victim's experience (capped by
`CONFIG_MAX_EXP_LOSS`), clears a player's killer and thief flags and calls
`raw_kill()`, which:

1. stops the fight and removes every affect;
2. stands the victim up so that commands in its death script work, and runs
   the death trigger (a 0 return suppresses the death cry);
3. runs the mob-kill quest checks for the killer, or for every member of the
   killer's group in the same room or zone;
4. tells the victim's group;
5. makes the corpse and calls `extract_char()` (deferred);
6. runs the save-mob and clear-room quest checks for the killer.

`make_corpse()` creates an unprototyped container named after the victim
with a decay timer (`CONFIG_MAX_NPC_CORPSE_TIME` or
`CONFIG_MAX_PC_CORPSE_TIME` ticks), moves the inventory into it wholesale,
moves each worn item in after running its remove trigger, adds a money pile
(for NPCs and connected players only, which stops a link-dead duplication
trick), and puts it in the room. The victim's own body is freed at the end
of the pulse.

### Invariants

- A character is on `combat_list` exactly when `FIGHTING(ch)` is set; only
  `set_fighting()` and `stop_fighting()` change either.
- Code that removes characters from `combat_list` during
  `perform_violence()` must use `stop_fighting()`, which advances the loop
  cursor.
- After `damage()` returns -1 the victim is pending extraction: do not hit,
  move or reward it again.
- `hit()` and `damage()` run scripts: re-read the weapon, the opponent and
  their rooms after them.

## Magic

### The spell table

`spell_parser.c:spell_info[]` has one entry per spell or skill number (1 to
`TOP_SPELL_DEFINE`). `mag_assign_spells()` marks every entry unused and then
defines each spell with `spello()`: mana cost range, minimum position,
targets (`TAR_*`), whether it is violent, its template routines (`MAG_*`)
and its wear-off message. Skills are defined with `skillo()` and have no
routines. `class.c:init_spell_levels()` then sets the level at which each
class learns each spell or skill with `spell_level()`. A spell no class may
use keeps `LVL_IMMORT`.

### Casting

```
do_cast (players)          parse 'spell name' and target, level and skill checks,
                           target search by TAR_* flags, mana, skill roll
  -> cast_spell()          position, charm, self-only/not-self, group checks,
                           say_spell() (the words, obfuscated for other classes)
      -> call_magic()      cast triggers (room, object, mob) may cancel;
                           NOMAGIC and PEACEFUL rooms; MOB_NOKILL targets;
                           then each MAG_* template the spell has, then the
                           manual spell if MAG_MANUAL
```

NPCs and special procedures enter at `cast_spell()`; scripts (`dg_cast`),
magic items and other code enter at `call_magic(caster, cvict, ovict,
spellnum, level, casttype)`. The cast type selects the saving throw
(`SAVING_SPELL` for spells, `SAVING_ROD` for items). A failed skill roll in
`do_cast` costs half the mana, and a violent spell aimed at an NPC makes the
NPC attack.

`do_cast` copies the target word into the global `cast_arg2` for spells
such as locate object. Only `do_cast` sets it, so a locate object cast from
a scroll or a script reuses whatever a player last typed.

### Templates and manual spells

| Routine | Function | What it does |
| --- | --- | --- |
| `MAG_DAMAGE` | `mag_damage()` | Computes damage by spell, halves it on a save, calls `damage()`. A return of -1 (victim died) stops `call_magic()`. |
| `MAG_AFFECTS` | `mag_affects()` | Builds up to five `affected_type` entries and joins them with `affect_join()`; refuses to stack a non-accumulating spell, or to grant an NPC a bit it already has from its prototype. |
| `MAG_UNAFFECTS` | `mag_unaffects()` | Removes affects (cure blind, remove poison, ...). |
| `MAG_POINTS` | `mag_points()` | Heals hit points or movement. |
| `MAG_ALTER_OBJS` | `mag_alter_objs()` | Changes an object (bless, curse, invisibility, poison). |
| `MAG_GROUPS` | `mag_groups()` | Applies a spell to every group member in the room, caster last. |
| `MAG_MASSES` | `mag_masses()` | Every character in the room (no spell uses it). |
| `MAG_AREAS` | `mag_areas()` | Damages everyone in the room except the caster, immortal players, other players when player-killing is off and the caster is a player, a player caster's charmed pets, and players in the caster's group (earthquake also spares fliers). |
| `MAG_SUMMONS` | `mag_summons()` | Clone and animate dead: loads a charmed follower, runs its load trigger, makes it follow the caster and joins it to the caster's group if the caster leads one. |
| `MAG_CREATIONS` | `mag_creations()` | Create food. |
| `MAG_ROOMS` | `mag_rooms()` | Darkness: flags the room dark and starts a mud event that removes the flag. |
| `MAG_MANUAL` | `spells.c` | Charm, create water, detect poison, enchant weapon, identify, locate object, summon, word of recall, teleport. |

The manual spells that move characters (recall, teleport, summon) call
`char_from_room()`/`char_to_room()`, show the room and run the mobile
memory and greet triggers, but ignore a greet refusal and do not run room
enter triggers.

### Magic items

`mag_objectmagic(ch, obj, arg)` handles staves (every character in the room,
or the room once per occupant for area spells), wands (one target, or the
room for area spells), scrolls (up to three spells, then the scroll is
extracted) and potions (consume trigger, up to three spells on the drinker,
then extracted). Charges are value 2 for staves and wands. Every spell is
cast through `call_magic()` and can run triggers, so the item, the target
and the next room occupant can all disappear while the item is being used;
in this branch the scroll and potion are extracted afterwards without
checking that they still exist.

### Invariants

- Every spell goes through `call_magic()`; put new restrictions there, not
  in `do_cast`.
- `call_magic()` runs scripts before it acts. After it returns, a
  character argument may be pending extraction and an object argument may
  be freed.
- Index `spell_info[]` only with a number checked against
  `TOP_SPELL_DEFINE`, and `min_level[]` only with a class checked against
  `NUM_CLASSES`.

## Mobile AI

`mobact.c:mobile_activity()` runs every 10 seconds and walks
`character_list`. For each NPC with a prototype:

1. If it has `MOB_SPEC`, special procedures are enabled and it is not
   pending extraction, its special procedure runs with `cmd` 0; a non-zero
   return ends its turn. (A `MOB_SPEC` mob without a procedure loses the
   flag with a log line.)
2. A fighting or sleeping mob does nothing else.
3. **Hunting**: `graph.c:hunt_victim()` takes one step toward `HUNTING(ch)`
   and attacks on arrival.
4. **Scavenging** (`MOB_SCAVENGER`): one time in eleven, picks up the most
   valuable object it can.
5. **Wandering**: unless `MOB_SENTINEL`, a standing mob tries a random
   direction about half the time, avoiding `NOMOB` and death rooms, and its
   own zone if `MOB_STAY_ZONE`. Charmed mobs do not wander.
6. **Aggression**: attacks the first visible player matching its
   `MOB_AGGRESSIVE`/`AGGR_EVIL`/`AGGR_NEUTRAL`/`AGGR_GOOD` flags (no-hassle
   players excepted; `MOB_WIMPY` attacks only sleepers). The test that
   should exclude blind and charmed mobs reads
   `!AFF_BLIND || !AFF_CHARM`, so only a mob that is both is excluded.
7. **Memory** (`MOB_MEMORY`): attacks a remembered player it can see.
8. **Charm rebellion**: a charmed mob whose master has too many charmed
   followers for their charisma may attack the master and stop following.
9. **Helping** (`MOB_HELPER`): joins an NPC's fight against a player.

`aggressive_mob_on_a_leash()` gives a charmed mob's master a charisma-based
chance to restrain it.

Mob memory is a list of player idnums (`memory_rec`) managed by
`remember()`, `forget()` and `clearMemory()`; it is separate from DG memory
(`mremember`), which lives in `ch->memory`.

The loop is safe against extraction because extraction is deferred, but
in this branch it checks `MOB_NOTDEADYET` only before the special procedure:
a mob purged by a script during hunting, moving or attacking keeps acting
for the rest of its turn.

## Regeneration, ticks and timers

`limits.c:point_update()` runs once per tick:

- For every character: hunger, thirst and drunkenness drop by one (players
  only; -1 means "never"). A character at `POS_STUNNED` or better regains
  hit points, mana and movement (`hit_gain()`, `mana_gain()`,
  `move_gain()`, which depend on level, class, position, hunger and thirst)
  and takes poison damage; incapacitated and mortally wounded characters
  take 1 or 2 points of suffering damage instead. A death here skips the
  rest of that character's update. Players' light sources burn down and
  object timers age (`update_char_objects()`), and idlers are handled by
  `check_idling()`.
- For every object: a corpse counts its timer down and, at 0, spills its
  contents into its container, its carrier's room or its own room and is
  extracted; any other object with a timer counts down and fires its timer
  trigger at 0.
- The happy-hour counter runs down.

`check_idling()` moves a player idle for `CONFIG_IDLE_VOID` ticks to room
**rnum** 1 (remembering the old room in `GET_WAS_IN()`), and after
`CONFIG_IDLE_RENT_TIME` ticks to rnum 3, detaches the descriptor, rent-saves
and extracts the character. Typing any command brings a voided player back
(`comm.c` game loop). Those rnums are hard-coded and name different rooms
once OLC inserts or deletes rooms below them.

`gain_exp(ch, gain)` caps gains at `CONFIG_MAX_EXP_GAIN` (with the happy-hour
bonus) and levels a player up through `class.c:advance_level()` while the
experience reaches `level_exp()` of the next level, never past
`LVL_IMMORT - 1`. At level
`LVL_IMMORT - 1` (the mortal cap, 50) experience is converted into
**endgame advancement points** instead (spent with `endgame`). Losses are
capped at `CONFIG_MAX_EXP_LOSS`. NPCs just accumulate experience.
`gain_exp_regardless()` (used by `advance`) has no caps and can reach any
level.

`increase_gold()`, `decrease_gold()`, `increase_bank()` and
`decrease_bank()` clamp coins to 0..`MAX_GOLD`/`MAX_BANK`; in this branch
they add in `int`, which can overflow near the cap.

The object timer is also counted down by `handler.c:update_char_objects()`
for objects a player carries (by 1) or wears (by 2), except objects with a
timer trigger. Carried corpses therefore decay faster than corpses on the
ground, and a carried object without a timer trigger can reach 0 there
without anything happening.

### Invariants

- `point_update()` calls `damage()`, `timer_otrigger()` and `act()`; any of
  them can run scripts that extract objects. The object pass must not
  follow a saved `next` pointer across them (in this branch it does).
- A player's `GET_WAS_IN()` holds a room while they are in the void; it is
  an rnum.

## Shops

A shop (`struct shop_data` in `shop_index[]`) is attached to a keeper mobile
at boot by `shop.c:assign_the_shopkeepers()`, which installs
`shop_keeper()` as the special procedure of the keeper's prototype and keeps
any procedure it replaced as the shop's secondary function (`SHOP_FUNC`).
Storage conventions matter when changing shops:

| Field | Holds |
| --- | --- |
| `keeper` | mobile **rnum** |
| `producing[]` | object **rnums** (despite the `obj_vnum *` type), `NOTHING`-terminated; converted from vnums at load |
| `in_room[]` | room **vnums**, `NOTHING`-terminated |
| `type[]` | item types the shop buys, each with an optional keyword expression |
| messages | templates expanded by `shop_format_message()`: only `%s` (customer name), `%d` (amount) and `%%`; never used as a `printf` format |
| `lastsort` (`SHOP_SORT`) | how many items at the head of the keeper's inventory are sorted |

`shop_keeper()` finds the shop by scanning for the keeper's rnum, runs the
secondary function first, ignores the keeper's own commands (resetting the
sort count on any command, in case the keeper dropped things), requires the
customer to be in one of the shop's rooms and the keeper to be awake, slaps
anyone who tries `steal`, and handles `buy`, `sell`, `value`, `list` and
`identify`. Opening hours, customer restrictions (class, alignment) and the
"keeper won't trade with you" rules are in `is_ok()`.

**Buying** finds the item in the keeper's inventory (`#n` or name; an item
with cost 0 or less is destroyed on sight), checks gold (or quest points for
`ITEM_QUEST` items) and carrying capacity, and then for each unit either
**clones** it with `read_object(rnum, REAL)` if the shop produces it, or
moves the keeper's copy to the buyer. Prices use the shop's profit factor
and the charisma difference between buyer and keeper. No DG triggers run,
and cloned items do not run load triggers. The keeper is paid only when
another unit of the same item remains (`obj` is non-NULL after the loop), so
buying a keeper's last non-produced item pays nobody.

**Selling** checks that the shop trades in the item's type
(`trade_with()`, including the keyword expression evaluated by
`evaluate_expression()`), that the keeper or the shop's bank can pay, and
moves the item into the keeper's inventory with `slide_obj()`, which keeps
identical items together and **extracts** an item identical to one the
shop produces. Shops with `HAS_UNLIMITED_CASH` never run out.

`ok_damage_shopkeeper()` (called from `damage()`) protects keepers of shops
without the "may be attacked" flag. `show shops` (`show_shops()`) lists
shops for immortals. The shop editor and the shop file writer are in
[olc.md](olc.md).

### Invariants

- Product lists hold rnums and must be renumbered when object prototypes are
  inserted or deleted ([olc.md](olc.md#renumbering-on-insert-and-delete)).
- `SHOP_SORT` counts sorted items at the head of the keeper's inventory;
  any code that adds to or removes from that inventory other than through
  `slide_obj()`/`sort_keeper_objs()` must keep it no larger than
  `IS_CARRYING_N(keeper)`.
- Builder-written shop text is data, never a format string.

## Quests

Quests (`struct aq_data` in `aquest_table[]`, accessed with the `QST_*`
macros) are offered by questmaster mobiles. `quest.c:assign_the_quests()`
installs `questmaster()` as the special procedure of each quest's
questmaster, parking the previous procedure in that quest's `QST_FUNC`
(only the first quest found for a questmaster keeps it), and
`questmaster()` consults only the first quest it finds for itself. The
`quest` command is `do_quest`, which handles `history`, `leave`,
`progress` and `status` anywhere; `list` and `join` need a questmaster in
the room, whose `questmaster()` procedure takes the command first.

A player has at most one active quest: `GET_QUEST(ch)` (a vnum, `NOTHING`
when none), its type, a counter of targets still needed, and a time limit
in ticks (-1 for none), counted down by `check_timed_quests()`.
Non-repeatable completed quests are kept in a growable array of vnums
(`add_completed_quest()`).

Progress is detected by `autoquest_trigger_check(ch, vict, obj, type)`,
called from the handler and combat code rather than from scripts:

| Quest type | Detected in |
| --- | --- |
| `AQ_OBJ_FIND` (get an object) | `obj_to_char()` |
| `AQ_ROOM_FIND` (reach a room), `AQ_MOB_FIND` (find a mobile) | `char_to_room()` |
| `AQ_MOB_KILL` | `raw_kill()`, for the killer or the killer's group |
| `AQ_MOB_SAVE` (only the protected mobile, or charmed ones, still in the room), `AQ_ROOM_CLEAR` (no NPC left in the target room) | after `raw_kill()` |
| `AQ_OBJ_RETURN` (give an object to a mobile) | `act.item.c:perform_give()`; the object is then **extracted** |

`generic_complete_quest()` decrements the counter and, at 0, pays quest
points, gold, experience and a reward object (with happy-hour bonuses),
records the quest as completed unless it is repeatable, clears it, starts
the follow-on quest if there is one, and saves the player.

Because the checks run inside `obj_to_char()` and `char_to_room()`, quest
completion happens in the middle of ordinary moves. In this branch two
re-entry problems follow: the reward object is given with `obj_to_char()`
while the quest is still active, so an object quest whose reward is its own
target completes again recursively; and `AQ_MOB_FIND` completes once per
matching mobile in the room, reading the quest table with `NOTHING` after
the first completion clears the quest.

The quest editor and the quest file writer are in [olc.md](olc.md).

## Special procedures

A special procedure is C code attached to a mobile, object or room
prototype:

```c
#define SPECIAL(name) int (name)(struct char_data *ch, void *me, int cmd, char *argument)
```

`me` is the mobile, object or room the procedure belongs to; `ch` is the
character acting; `cmd` is the command number, or 0 for a periodic call;
`argument` is the rest of the command line (writable, the caller's
buffer). Return non-zero to say "I handled this command" (the command is
then not run), zero to let it continue. Use `CMD_IS("name")` to test the
command.

Calls:

| Caller | `cmd` | Who |
| --- | --- | --- |
| `interpreter.c:special()` before every command | the command | in order: the actor's room, each piece of the actor's equipment, each item in the inventory, each mobile in the room (skipping mobiles pending extraction), each object in the room |
| `act.movement.c:do_simple_move()` for followers | `dir + 1` | the same |
| `mobact.c:mobile_activity()` every 10 s | 0 | mobiles with `MOB_SPEC` |
| `fight.c:perform_violence()` every round | 0 | fighting mobiles with `MOB_SPEC` |

Procedures are assigned per **prototype** at boot: `spec_assign.c:
assign_mobiles()`, `assign_objects()`, `assign_rooms()` (with
`ASSIGNMOB`/`ASSIGNOBJ`/`ASSIGNROOM` by vnum), followed by
`assign_the_shopkeepers()` and `assign_the_quests()`, which wrap any
procedure already there. `assign_rooms()` also makes every death-trap room
a `dump` when `CONFIG_DTS_ARE_DUMPS` is set. The `-s` command-line option
(`no_specials`) skips assignment, so no special procedure is ever called. Command-driven
calls happen whether or not `MOB_SPEC` is set; periodic calls need it
(builders set it in the mobile file; `spell_charm` clears it on charmed
mobs).

What is assigned in stock data: guildmasters (`guild`, which also implements
`practice`), receptionists, postmasters, the cryogenicist, the Midgaard
mayor, bulletin boards (`gen_board`), banks, pet shops, dumps, shopkeepers,
questmasters, and the King's Castle mobiles (`castle.c`, whose mobile and
room vnums are offsets from the zone's bottom vnum). Most of the classic
mobile procedures in `spec_procs.c` (`fido`, `janitor`, `cityguard`,
`snake`, `thief`, `magic_user`, `puff`, `guild_guard`) are no longer
assigned; DG triggers replaced them. `get_spec_func_name()` names
procedures for `stat` and returns NULL for any procedure not in its table
(including the castle procedures).

`special()` walks inventories and room lists and calls procedures that may
themselves move or extract things; it stops at the first procedure that
returns non-zero, and a procedure that returns zero after destroying
something leaves the walk with a dangling pointer. A procedure that acts
must return non-zero.

`pet_shops()` finds its pets in the room **rnum + 1**, which is the next
room vnum only until OLC inserts or deletes rooms.

## Events and generic lists

### lists.c

`struct list_data` is a doubly linked list of `struct item_data` nodes,
each holding a `void *` content. The list does **not** own its contents.
Every list is also registered in `global_lists` for debugging.

| Function | Notes |
| --- | --- |
| `create_list()`, `free_list()` | `free_list()` frees the nodes and the header, never the contents. |
| `add_to_list(content, list)` | Appends. Duplicates are allowed. |
| `remove_from_list(content, list)` | Finds the first node with that content and frees it; logs if absent. |
| `merge_iterator(&it, list)`, `next_in_list(&it)`, `remove_iterator(&it)` | A caller-owned iterator. `merge_iterator()` returns the first content and counts an iterator on the list. |
| `simple_list(list)` | A convenience iterator with **one hidden global cursor**: call it in a `while` loop until it returns NULL; `simple_list(NULL)` or `clear_simple_list()` resets it. |
| `random_from_list()`, `randomize_list()` | Random selection; `randomize_list()` frees the old list and returns a new one. |

`simple_list()` is the trap. Its cursor is a file-static `Iterator`, and in
this branch `find_in_list()` (used by `remove_from_list()`) reuses and
resets that same `Iterator`. Consequences:

- **Removing from a list inside a `simple_list()` loop ends the loop after
  the current element.** `free_list()` itself drains with that pattern, so
  in this branch it frees only the first node, logs "List being freed while
  not empty", and leaks the rest. Every owner teardown loop that cancels
  events with `while ((e = simple_list(owner->events))) event_cancel(e);`
  cancels only the first event.
- **Nested `simple_list()` loops corrupt each other.** Starting a loop on a
  different list resets the cursor (logged as "simple_list() forced to reset
  itself"); breaking out of a loop early leaves the cursor parked for the
  next caller. Any loop body that can run a script, a spell or combat must
  use a private iterator (`merge_iterator()`), because those can start
  another `simple_list()` loop (group code, `raw_kill()`, `mag_groups()`).

### mud_event.c

Mud events are a typed layer over the DG queue (`dg_event.c`, whose
mechanics are in [dg-scripts.md](dg-scripts.md#the-event-queue)).
`mud_event_index[]` maps an event id to its callback and owner type:

| Id | Callback | Owner | Used by |
| --- | --- | --- | --- |
| `ePROTOCOLS` | `get_protocols()` | descriptor | protocol negotiation 1.5 s after connecting ([networking.md](networking.md)) |
| `eWHIRLWIND` | `event_whirlwind()` | character | the whirlwind skill, repeating every 1.5 s |
| `eSPL_DARKNESS` | `event_countdown()` | room | darkness expiry after 5 s |

`NEW_EVENT(id, owner, string, pulses)` creates a `struct mud_event_data`
(owner pointer in `pStruct`, an optional copied string) and
`attach_mud_event()` queues it and appends the event to the owner's
`events` list (for characters and rooms, creating the list on first use;
descriptors get theirs when they connect) or to `world_events`. The
callback returns the delay before it runs again, or 0 to finish; the engine
then frees the payload with `free_mud_event()`, which unlinks the event from
the owner's list, **frees the owner's list and sets it to NULL when it
becomes empty**, and frees the string and the payload.
`char_has_mud_event(ch, id)` finds a character's event of a given id.

Owners cancel their events when they go away: `extract_char()` calls
`clear_char_event_list()`, and `free_char()`, `extract_obj()`,
`genwld.c:delete_room()`, `comm.c:close_socket()` and `db.c:destroy_db()`
cancel and free their lists.

Lifetime rules and the known weak points:

- **The owner pointer is raw.** A room event stores `&world[rnum]`; OLC can
  reallocate `world[]` (`add_room()`/`delete_room()`), and nothing repoints
  mud events, so a darkness event can fire on freed memory.
- **Do not cancel the running event.** While a callback runs, its event has
  been taken off the queue (`q_el == NULL`). `event_whirlwind()` calls
  `hit()`; if anything reached from there extracts the whirlwinding
  character (a damage script that kills them, for example),
  `extract_char()` cancels the event that is still running, freeing it
  under `event_process()`. The engine-side fix (a cancelled running event is
  left for `event_process()` to free) is pending on a D2 branch; see
  [dg-scripts.md](dg-scripts.md#pending-changes-on-audit-fix-branches).
- **Teardown after the last event.** Because `free_mud_event()` frees the
  owner's list when the last event goes, an owner loop that cancels events
  and then calls `free_list(owner->events)` passes NULL to `free_list()`.
- The event list of a character must not be walked with `simple_list()`
  while events are cancelled (see lists.c above).

### Invariants

- Lists never own their contents; free the contents yourself.
- Only one `simple_list()` loop may be active at a time in the whole
  server, and its body must not remove from the list or run anything that
  could start another loop.
- A mud event's owner must cancel the event before the owner is freed or
  moved, and nothing reached from a callback may cancel that same event.

## Pathfinding

`graph.c:find_first_step(src, target)` does a breadth-first search over the
room graph and returns the first direction of a shortest path, or
`BFS_ALREADY_THERE`, `BFS_NO_PATH` or `BFS_ERROR`. It marks visited rooms
with the room flag `ROOM_BFS_MARK`, clearing the flag on **every room** at
the start of each search (so marks left behind, even ones saved by OLC, do
no harm), and keeps its queue in heap nodes freed before it returns. Exits
to `NOWHERE`, `ROOM_NOTRACK` rooms and (unless `CONFIG_TRACK_T_DOORS`)
closed doors are not followed. Each call costs time proportional to the
whole world.

`do_track` (the thief skill) prints the direction, or a random one on a
failed skill roll. `hunt_victim()` (from `mobile_activity()`) checks that
`HUNTING(ch)` is still on `character_list`, takes one step with
`perform_move()` and attacks if it arrives. `extract_char_final()` clears
`HUNTING()` of every NPC hunting a character being freed.

## Weather and time

`weather.c:weather_and_time()` runs every tick. `another_hour()` advances
`time_info` (24 hours a day, 35 days a month, 17 months a year), sets
`weather_info.sunlight` and announces sunrise (5), day (6), sunset (21) and
night (22) to players outdoors. `weather_change()` moves a single global
barometric pressure (with a seasonal bias) and derives the sky:
cloudless, cloudy, raining or lightning, announcing changes outdoors.
Weather is world-wide; there are no regional climates. The clock is saved
every 30 minutes (`save_mud_time()`) and restored at boot.

## The ASCII map

`asciimap.c` draws maps from room exits. `can_see_map()` checks
`CONFIG_MAP` (off, on, or immortals only). `MapArea()` recursively paints a
static 51x51 integer grid, `map[][]`, with sector types and door glyphs,
starting from the viewer's room and following exits up to a given
distance; `StringMap()`, `WorldMap()` and `CompactStringMap()` turn the grid
into text using glyph tables indexed by sector type. `do_map` prints a map
with a legend; `str_and_map()` prints a room description with a small map
beside it (`PRF_AUTOMAP`, called from `look_at_room()`), sized by
`CONFIG_MINIMAP_SIZE`.

The grid and the output buffers are static: the map code is not reentrant,
and its sizes must be kept within the grid. Sector values come from world
files and index the glyph tables directly.

## Classes and levels

`class.c` holds the tables that differ by class. There are five player
classes: magic user (shown as Wizard; selected with `z` or the legacy `m`),
cleric, thief, warrior and monk (`CLASS_MONK`, 4). The numeric class ids are
stored in player files and must never change meaning.

| Function or table | Purpose |
| --- | --- |
| `parse_class()`, `find_class_bitvector()`, `class_abbrevs[]`, `pc_class_types[]`, `class_menu` | Names and selection at character creation |
| `prac_params[][NUM_CLASSES]` | Practice gains and the "learned" cap per class |
| `guild_info[]` | Which guild guards block which classes |
| `saving_throws(class, type, level)` | Base saving throws |
| `thaco(class, level)` | To-hit base for players |
| `roll_real_abils()` | Rolls and orders the six abilities by class |
| `do_start()` | Level 1 setup: abilities, starting points and skills, full conditions |
| `advance_level()` | Per-level hit point, mana, movement and practice gains; immortals stop needing food and get holylight |
| `backstab_mult()` | Backstab multiplier by level |
| `invalid_class()` | `ITEM_ANTI_<class>` checks used by `equip_char()` |
| `init_spell_levels()` | Which class learns which spell or skill at which level |
| `level_exp(class, level)` | Experience needed for a level |
| `title_male()`, `title_female()` | Default titles |

Levels 1 to 50 are mortal; 51 (`LVL_IMMORT`), 55 (`LVL_GOD`), 58
(`LVL_GRGOD`) and 60 (`LVL_IMPL`) are the immortal ranks; builders are
`LVL_BUILDER` (51). See [../../doc/levels-50-60.md](../../doc/levels-50-60.md)
and [../../doc/class-skilltree-imports.md](../../doc/class-skilltree-imports.md).

The tables are indexed by `GET_CLASS(ch)`, which is read from the player
file; index them only with a class checked against `NUM_CLASSES`.

## Invariants checklist

**Lifetime**

1. Characters are freed at the end of the pulse (`extract_pending_chars()`);
   objects are freed inside `extract_obj()`. Code that continues after a
   callback re-validates characters by `NOTDEADYET` flags and room, objects
   by UID (`obj_uid_matches()`).
2. Callbacks include every DG dispatcher, `act()` (act triggers), every
   call to `command_interpreter()`, `hit()`, `damage()`, `call_magic()`,
   special procedures and mud event callbacks.
3. Never follow a `next_content`, `next_in_room`, `next` or `k->next`
   pointer saved before a callback; re-acquire from the list head or by
   UID.
4. `follow_type` nodes and affects are freed immediately when removed.
5. Do not cancel a mud event from inside its own callback chain.

**Placement**

6. An object is in at most one place; every move is a `from` followed by a
   `to`, and nothing returns to the game loop in limbo.
7. Weight, counts, light and affect totals are maintained only by the
   `handler.c` primitives.
8. `char_from_room()` and `extract_char_final()` exit the server for a
   character in `NOWHERE`; `char_to_room()` silently does nothing for an
   invalid room. Check rooms before calling them.

**Combat**

9. `FIGHTING(ch)` is set exactly while `ch` is on `combat_list`; only
   `set_fighting()`/`stop_fighting()` change them, and only
   `stop_fighting()` may remove someone during `perform_violence()`.
10. After `damage()` returns -1 the victim is dead and pending extraction.

**Lists and events**

11. Lists do not own their contents.
12. `simple_list()` is a single global cursor: no nesting, no removal inside
    the loop, no callbacks inside the loop body.
13. Mud event owner pointers are raw; anything that moves or frees an owner
    must cancel or repoint its events first.

**Tables and numbers**

14. Room, object and mobile **rnums** change when OLC inserts or deletes
    prototypes. Hold vnums across anything that can run OLC, and never hold
    `&world[n]`.
15. Index class tables with a checked class, spell tables with a checked
    spell number, and glyph or message tables with checked world-file
    values.
16. Builder text (shop messages, socials, act messages) is data, never a
    format string.

## Pending changes on audit fix branches

None of the changes below is in this branch. Each was read on the branch
named, which is the branch `audit-results-20261027/merge-plan.json` names
for the chunk (H1: `worktree-wf_c613c2c5-f9f-43`, H2:
`worktree-wf_c613c2c5-f9f-44`, H3: `audit-fix-H3`). The commit messages
carry the full reasoning and the regression tests.

**`worktree-wf_c613c2c5-f9f-43`** (H1: `act.item.c`, `act.movement.c`,
`act.offensive.c`, `fight.c`, `magic.c`):

| Commit | Change |
| --- | --- |
| `8ddabce` | The `get`, `put`, `drop`, `give` and `wear` loops resume from a UID mark after each trigger instead of a saved `next_content`. |
| `4547bc7` | `hit()` reads the wielded weapon after the fight trigger and stops (with a warning) if the trigger purged either fighter. |
| `192e017` | `make_corpse()` moves the inventory before running remove triggers. |
| `69123cc` | Door commands re-validate the exit or container after door triggers. |
| `451a303` | Animate dead re-validates the corpse after the zombie's load trigger. |
| `255afab` | Peaceful rooms stop players even when the DG proxy mobile does not exist (in this branch a missing vnum 1 lets players fight anywhere). |
| `4d0e1eb` | `set_fighting()`, `stop_fighting()` and `perform_violence()` unseat characters from furniture when they change position. |
| `7d39533` | Documents the follower-loop hazard in `perform_move()`; it does not change the loop. |

**`worktree-wf_c613c2c5-f9f-44`** (H2: `handler.c`, `shop.c`,
`spell_parser.c`, `spells.c`):

| Commit | Change |
| --- | --- |
| `e2a0deb` | `call_magic()` re-validates caster, victim and target object after each cast trigger and stops if one was purged. |
| `1594e39` | Scrolls and potions are not extracted a second time after a cast trigger purged them. |
| `0081305` | `obj_from_room()` clears the furniture's sitter list and count when it unseats sitters. |
| `1c3e16c` | `equip_char()` rejections name the caller and never leave the object in limbo. |
| `db4dc69` | `affect_join()` clamps stacked modifiers and durations instead of wrapping. |
| `eda12c3` | Shop keyword expressions deeper than the evaluator's stack are rejected. |

**`audit-fix-H3`** (H3: `lists.c`, `mud_event.c`, `limits.c`, `mobact.c`,
`quest.c`, `spec_procs.c`):

| Commit | Change |
| --- | --- |
| `a2efc45` | `simple_list()` prefetches its next item and `find_in_list()` no longer touches its cursor, so removal inside a loop is safe; `free_list()` frees every node. |
| `9c5a87a` | Cancelling an owner's last mud event no longer frees the owner's list; `free_list(NULL)` is reported instead of crashing. |
| `b671317` | Room mud events record the room vnum and resolve it with `real_room()` when they fire. |
| `419d5d3` | A mobile purged during its turn stops acting for the rest of the pulse (`MOB_STILL_ACTING`). |
| `5328ded` | `point_update()` snapshots expired objects as (address, UID) pairs and re-validates each before running its timer or decay. |
| `c283557` | Find-mob quests complete once per quest, not once per matching mobile. |
| `a8f2df4` | A quest is recorded and cleared before its rewards are paid, so a reward cannot complete it again. |
| `0161ec0` | A questmaster's original special procedure is recorded on every quest it offers and found from any of them. |
| `8358295` | Idle players go to rooms named by vnum; orphaned corpse contents are reported and extracted. |
| `84ff749` | Coin arithmetic is done in `long long` and clamps are reported; the bank refuses transfers that would be clamped. |
| `467d46d` | Pet shops find their storage room by vnum (shop vnum + 1) and refuse a missing one. |

## Further reading

- [overview.md](overview.md): the game loop, boot order and pulses.
- [dg-scripts.md](dg-scripts.md): the trigger engine, especially
  [Callback safety](dg-scripts.md#callback-safety) and
  [The event queue](dg-scripts.md#the-event-queue).
- [olc.md](olc.md): editors, and what saving a prototype does to live
  instances and stored rnums.
- [persistence.md](persistence.md): world loading, zone resets, player and
  object saving.
- [networking.md](networking.md): descriptors, connection states and output.
- [foundations.md](foundations.md): `structs.h`, `utils.h` macros and the
  error reporters.
- [../maintainer-safety-guide.md](../maintainer-safety-guide.md): diagnostic
  conventions and the callback lifetime rules.
- Legacy references: [../../doc/coding.txt](../../doc/coding.txt) (Jeremy
  Elson's guide to commands, spells, skills and special procedures, still
  accurate in outline), [../../doc/dg_events.txt](../../doc/dg_events.txt)
  (the event systems) and [../../doc/act.txt](../../doc/act.txt) (the `act()`
  format codes).
