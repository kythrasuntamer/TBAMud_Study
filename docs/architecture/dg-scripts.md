# DG Scripts: the trigger engine

DG Scripts (the "Death's Gate" scripting system) let builders make mobiles,
objects and rooms react to the game without writing C. A builder writes a
*trigger*: a name, a type, a numeric and a text argument, and a list of
command lines. The trigger is attached to a prototype or to a live entity.
When its event happens, the engine runs the lines one at a time: it expands
`%variables%`, evaluates `if`, `while` and `switch` blocks itself, and hands
every other line to a command interpreter for that kind of owner.

This document explains how that engine is built, for maintainers who will
change the C code: the data structures and who owns them, how triggers are
loaded, attached and dispatched, how a script runs, how variables are
expanded, how `wait` uses the event queue, how scripts name entities by
unique ID, and the rules that stop a script from leaving a dangling pointer.
The builder's language reference is the in-game help (`help trigedit` and the
pages it links). The safety conventions are in
[../maintainer-safety-guide.md](../maintainer-safety-guide.md); this document
summarizes them where they apply and links there.

All statements describe the code at the head of this branch. Changes that
exist only on the audit fix branches are listed separately in
[Pending changes on audit fix branches](#pending-changes-on-audit-fix-branches).

## Source files

| File | Role |
| --- | --- |
| `src/dg_scripts.h` | Trigger type bits, the trigger/script/variable structs, accessor macros (`GET_TRIG_*`, `SCRIPT`, `TRIGGER_CHECK`, `ADD_UID_VAR`), UID bases, prototypes for every DG file. |
| `src/dg_scripts.c` | The interpreter `script_driver()`, the expression evaluator, the control-flow scanners, the script commands (`wait`, `set`, `eval`, `global`, `remote`, `attach`, ...), name and UID resolvers, the UID lookup table and ID allocation, saved player variables, the periodic random/time checks, and the immortal commands `attach`, `detach`, `tstat`, `vdelete`. |
| `src/dg_triggers.c` | One dispatcher per trigger type (`greet_mtrigger()`, `get_otrigger()`, `command_wtrigger()`, ...), the snapshot helpers, and the word/phrase matchers. |
| `src/dg_variables.c` | Variable storage (`add_var()`, `add_player_var()`), `%...%` expansion (`var_subst()`), field resolution (`find_replacement()`), string fields (`text_processed()`), player-text neutralization. |
| `src/dg_handler.c` | Freeing triggers, scripts, variable lists and prototype trigger lists; deferred freeing while scripts run; repointing room waits. |
| `src/dg_event.c`, `src/dg_event.h` | The bucketed priority queue behind `wait` and the typed mud events. |
| `src/dg_db_scripts.c` | Parsing `.trg` files, instantiating triggers from prototypes, reading `T` lines, `assign_triggers()`. |
| `src/dg_mobcmd.c` | Mobile commands (`mecho`, `mload`, `mpurge`, `mtransform`, ...). |
| `src/dg_objcmd.c` | Object command table, `obj_command_interpreter()`, the `o*` commands, `obj_room()`. |
| `src/dg_wldcmd.c` | Room command table, `wld_command_interpreter()`, the `w*` commands. |
| `src/dg_comm.c` | `sub_write()` (the `~name` message tokens), `send_to_zone()`, `any_one_name()`. |
| `src/dg_misc.c` | `dg_cast` and `dg_affect`, `script_damage()`, `valid_dg_target()` (the immortal targeting policy). |
| `src/dg_olc.c`, `src/dg_olc.h` | The trigger editor (`trigedit`), the attached-trigger editor shared by `medit`, `oedit` and `redit`, the script formatter. |

Engine code outside these files: `interpreter.c:command_interpreter()` (fires
command triggers) and `interpreter.c:script_command_interpreter()` (the
mobile command table), `comm.c:heartbeat()` (event processing, random and
time checks), `comm.c:perform_act()` (act triggers),
`interpreter.c:enter_player_game()` (player UID registration, saved
variables, login triggers), and `db.c:free_char()`/`db.c:free_obj()` (UID
removal).

## The model in one picture

```
 PROTOTYPE SIDE (boot, OLC)                 INSTANCE SIDE (running game)

 trig_index[rnum] -> index_data             mob / obj / room
                       .vnum                   .proto_script -> trig_proto_list -> ...  (vnums)
                       .number (live count)    .script -------> script_data
                       .proto -> trig_data                        .trig_list -> trig_data -> trig_data
                                  .cmdlist ---+                   .global_vars    | .var_list (locals)
                                              |                   .context        | .wait_event -> event
                                              +--- shared, not copied ----------> | .cmdlist
                                                                                  | .curr_state, .depth

 trigger_list: every live trig_data, linked through next_in_world
```

Five facts carry most of the design:

1. **A trigger instance shares its prototype's command list.** `trig_data_copy()`
   copies the name and argument but only the *pointer* to `cmdlist`. Freeing an
   instance never frees the command lines; only `trigedit_save()` (and
   shutdown) does, after repointing every instance.
2. **A script is per owner.** Each mob, object or room with triggers has one
   `struct script_data`, which holds its trigger instances and its global
   variables. Players get one too (it holds their saved variables).
3. **`proto_script` is a list of vnums, not triggers.** Prototypes and
   instances both carry it; instances are built from it by `assign_triggers()`.
4. **Scripts name entities by UID**, a `long` with a namespace per entity kind
   (players, mobiles, rooms, objects), resolved through a hash table that
   stores raw pointers.
5. **Nothing a script frees is freed while any script is running.** Detached
   triggers and scripts are *retired* and freed when the outermost
   `script_driver()` frame returns. Characters are extracted at the end of the
   pulse. Objects, however, are freed immediately.

## Data structures

### `struct trig_data`: one trigger (prototype, instance or editor scratch)

| Field | Meaning and ownership |
| --- | --- |
| `nr` | The prototype's rnum in `trig_index`. `NOTHING` for an unsaved editor scratch trigger (`trigedit_setup_new()` writes `NOWHERE`, which has the same value). Must stay correct when prototypes are inserted (see [Trigger OLC](#trigger-olc)). |
| `attach_type` | `MOB_TRIGGER` (0), `OBJ_TRIGGER` (1) or `WLD_TRIGGER` (2): which owner kind the type bits are meant for. |
| `data_type` | Copied around but not used by the engine. |
| `name`, `arglist` | Owned strings, `strdup`ed per instance. `arglist` may be NULL. |
| `trigger_type` | Bitvector of `MTRIG_*`, `OTRIG_*` or `WTRIG_*` bits. |
| `cmdlist` | Head of the command lines. **Owned by the prototype**; instances and editor copies borrow it. |
| `curr_state` | The line to resume at after a `wait`. Only meaningful while waiting. |
| `narg` | The numeric argument (a percent chance for most types; see the dispatch table). |
| `depth` | 0 when the trigger is idle. While it runs (and while it waits) it is 1 plus the number of open `if` blocks. `TRIGGER_CHECK()` refuses to start a trigger whose depth is non-zero, so **a running or waiting trigger never fires again**. |
| `loops` | Iterations of `while` loops in the current run (reset at each new run). |
| `wait_event` | The queued event that will resume this trigger, or NULL. |
| `purged` | Initialized to FALSE and never read. |
| `var_list` | Local variables of the current run. Owned by the trigger; freed when the run ends (not when it waits). |
| `serial` | A never-reused number given by `trig_data_init()`. Lets a snapshot tell a trigger from a new one allocated at the same address. Scratch triggers made with plain `CREATE()` have serial 0. |
| `next` | Next trigger in the owner's script. |
| `next_in_world` | Next live trigger in the global `trigger_list`. |

### `struct script_data`: one owner's script

`types` is the OR of its triggers' type bits; dispatchers test it first with
`SCRIPT_CHECK()` so that an owner with no matching trigger costs one bit test.
`trig_list` holds the trigger instances in order (order matters: most
dispatchers run only the first trigger that matches). `global_vars` holds
variables that outlive a run; `context` selects which context-tagged globals a
run can see (see [Variables](#variables-and-substitution)). `purged` is unused;
`next` is unused since retirement moved to its own list.

`remove_trigger()` recomputes `types` after a removal; `add_trigger()` ORs the
new trigger's bits in.

### `struct cmdlist_element`: one command line

`cmd` is the line text. `original` and `loops` are scratch fields used by
`while`/`done` (see [Control flow](#control-flow)). They live in the shared
list, so every instance of a trigger shares them.

### `struct trig_var_data`: one variable

`name` and `value` are owned strings; `context` is 0 for an ordinary variable.
`add_var()` copies both strings, so callers keep ownership of what they pass.

### `struct trig_proto_list`, `struct script_memory`, `struct wait_event_data`

* `trig_proto_list` is a singly linked list of trigger vnums (`proto_script`).
  `copy_proto_script()` deep-copies it; `free_proto_script()` frees it.
* `script_memory` is a mobile's DG memory (`mremember`): a UID and an optional
  command string (owned). It lives in `ch->memory` (`SCRIPT_MEM(ch)`), not in
  the script, and is freed by `extract_script_mem()`.
* `wait_event_data` is the payload of a `wait` event: the trigger, the owner
  pointer `go`, and the owner type. It is owned by the event and freed by
  `trig_wait_event()` when the event fires, or by `event_cancel()`.

### Invariants

* `trig_index` is sorted by vnum; `real_trigger()` binary-searches it.
  `trig_index[GET_TRIG_RNUM(t)]->proto` is `t`'s prototype for every live
  trigger `t`.
* `trig_index[rnum]->number` counts live instances: `read_trigger()`
  increments it, `extract_trigger()` decrements it.
* Every live trigger is on exactly one owner's `trig_list` and on
  `trigger_list`. A retired trigger is on neither.
* An instance's `cmdlist` equals its prototype's `cmdlist`. Never free an
  instance's command list.

## Trigger types and attachment

### Owner kinds and type bits

The three families share bit positions but not meanings. `GLOBAL` (bit 0) is
not an event: it lets random and time triggers fire while no player is in the
zone. Type names come from `constants.c:trig_types`, `otrig_types` and
`wtrig_types`; unnamed bits print as `UNUSED`. Bit 18 is unused for mobiles.

| Bit | Mobile (`MTRIG_`) | Object (`OTRIG_`) | Room (`WTRIG_`) |
| --- | --- | --- | --- |
| 0 | Global | Global (unused) | Global |
| 1 | Random | Random | Random |
| 2 | Command | Command | Command |
| 3 | Speech | | Speech |
| 4 | Act | | |
| 5 | Death | Timer | Zone Reset |
| 6 | Greet | Get | Enter |
| 7 | Greet-All | Drop | Drop |
| 8 | Entry | Give | |
| 9 | Receive | Wear | |
| 10 | Fight | | |
| 11 | HitPrcnt | Remove | |
| 12 | Bribe | | |
| 13 | Load | Load | |
| 14 | Memory | | |
| 15 | Cast | Cast | Cast |
| 16 | Leave | Leave | Leave |
| 17 | Door | | Door |
| 18 | | Consume | Login |
| 19 | Time | Time | Time |
| 20 | Damage | | |

Object command triggers use `narg` as a second bitvector: `OCMD_EQUIP`,
`OCMD_INVEN` and `OCMD_ROOM` say where the object must be relative to the
person typing.

### Loading prototypes from disk

At boot `db.c` reads `lib/world/trg/index` and calls
`dg_db_scripts.c:parse_trigger()` for each record. A record looks like this
(trigger 100 in `lib/world/trg/1.trg`):

```
#100
Obj Command 100 - portal to Midgaard~
1 c 7
en~
if %cmd.mudcommand% == enter && portal /= %arg%
  %send% %actor% You enter the portal.
  %teleport% %actor% 3001
  ...
end
~
```

The second line is `attach_type flags narg`: an object trigger, type
`c` (bit 2, Command), and `narg` 7 (equipment, inventory and room). The third
is the text argument. The body is split on line breaks into the command list.
`parse_trigger()` exits the server on a malformed header line (fail fast at
boot). It sets `nr` to the record's position, so the index file must list
zone files in vnum order.

### Attaching: `proto_script`, `T` lines and `assign_triggers()`

Mobile, object and room records may carry `T <vnum>` lines.

* Mobiles and rooms: `dg_db_scripts.c:dg_read_trigger()` appends the vnum to
  the prototype's `proto_script`. A **room is its own prototype**, so a room
  trigger is also instantiated at once (`read_trigger()` + `add_trigger()`).
* Objects: `dg_db_scripts.c:dg_obj_trigger()` appends the vnum.
* A vnum with no trigger is reported to builders and dropped from the list.

Mobile and object instances get their triggers when they are created:
`db.c:read_mobile()` and `db.c:read_object()` struct-copy the prototype, then
call `copy_proto_script()` (each instance gets its own vnum list) and
`assign_triggers()`, which calls `read_trigger()` for each vnum and adds the
instance to a fresh `script_data`. `read_trigger()` allocates the instance,
copies the prototype with `trig_data_copy()` (new serial, shared `cmdlist`)
and increments the live count.

`add_trigger(sc, t, loc)` inserts `t` into `sc` at position `loc` (0 = first,
-1 = last), ORs in its type bits, and pushes it onto `trigger_list`.

### Attaching and detaching at run time

Two ways exist, with different checks:

* **Immortal commands** `attach` and `detach` (`dg_scripts.c:do_attach()`,
  `dg_scripts.c:do_detach()`, level `LVL_BUILDER`). They find the target by
  name or by vnum in the room/inventory. Attaching to a player requires
  `CONFIG_SCRIPT_PLAYERS`. Zone permission (`oasis.c:can_edit_zone()`, see
  [olc.md](olc.md#builder-permissions)) is checked against the *target room*
  for rooms, against the *target's prototype zone* for `detach` of mobiles
  and objects, but against the *immortal's current room* for `attach` to
  mobiles and objects.
* **Script commands** `attach <vnum> <uid>` and `detach <vnum|all> <uid>`
  (`dg_scripts.c:process_attach()`, `process_detach()`). The target is any
  UID; there is no zone check. A player target is refused unless
  `CONFIG_SCRIPT_PLAYERS` is on, and the refusal happens before an instance is
  created.

`remove_trigger()` accepts a position, a vnum, or a name with an optional
`N.` prefix (`2.beggar-death`). A plain number matches either a position or a
vnum, whichever comes first in the list. It writes into its `name` argument
(the `.` becomes a NUL). When the last trigger goes, the caller removes the
whole script with `extract_script()`.

### What OLC does to live scripts

Saving a mobile in `medit` or an object in `oedit` installs the edited vnum
list on the prototype and then, **for every live instance**, frees and
recopies its `proto_script` and runs `assign_triggers()`
(`medit.c:medit_save_internally()`, `oedit.c:oedit_save_internally()`). This
happens on every save, whether or not the trigger list changed. A mobile's
running script is extracted first, so its global variables and pending waits
are lost (its DG memory, which lives outside the script, is kept). For
objects the loop's `extract_script()` never runs: `genobj.c:add_object()` has
already overwritten every instance with the prototype
(`update_all_objects()`), replacing its `script` pointer with the
prototype's NULL, so the old script is orphaned rather than freed. Its
triggers stay on `trigger_list`, and one that was waiting resumes on the
object when the wait expires (see [olc.md](olc.md#oedit-object-prototypes)). A room save (`genwld.c:add_room()`) extracts the
room's script and `redit.c:redit_save_internally()` assigns the new list.
Details of the editor side are in [Trigger OLC](#trigger-olc).

### Invariants

* Only prototypes' `proto_script` lists are written to disk (by the OLC save
  functions through `dg_olc.c:script_save_to_disk()`).
* In this branch every vnum in a `proto_script` list resolves with
  `real_trigger()`: the loaders and the attached-trigger editor reject
  unknown vnums, and triggers are never deleted while the server runs.
  `dg_olc.c:dg_script_menu()` relies on this (it indexes `trig_index` with the
  result unchecked).
* An instance's `proto_script` is a private copy; `db.c:free_obj()`,
  `db.c:free_char()` and `handler.c:extract_obj()` free it only when it is
  not the prototype's pointer.

## Trigger dispatch

### The shape of a dispatcher

Every function in `dg_triggers.c` follows the same pattern:

1. `SCRIPT_CHECK(owner, TYPE)`: the owner has a script and some trigger of
   this type. Most mobile dispatchers also skip charmed mobs.
2. Walk the owner's triggers and pick the first one for which
   `TRIGGER_CHECK(t, TYPE)` holds (type bit set **and depth 0**) and, for most
   types, `rand_number(1, 100) <= narg`.
3. Seed local variables: `actor` and friends as UIDs with `ADD_UID_VAR()`,
   other values with `add_var()`, and anything typed or said by a player with
   `add_player_var()`.
4. Call `script_driver(&owner, t, TYPE, TRIG_NEW)` and return (or `break`).

Because a dispatcher usually runs only the first matching trigger, the order
of triggers in a script is significant. The exceptions that run several
triggers or several owners (greet, command, timer, leave on objects) use the
snapshot helpers described in [Callback safety](#callback-safety).

### Return values

`script_driver()` returns 1 unless the script ran `return <n>`. For the
triggers whose dispatcher returns `int`, **0 vetoes the action** that caused
the trigger. The caller decides what that means:

| Trigger | Caller | A return of 0 ... |
| --- | --- | --- |
| Command (mob, obj, room) | `interpreter.c:command_interpreter()` tries room, then mobs, then objects | is *not* a veto: non-zero means "the trigger handled the command"; 0 lets the normal command run. A script that never says `return 0` swallows the command. |
| Leave (mob, room, obj) | `act.movement.c:do_simple_move()` before the move | blocks the move. The mover is also kept if a trigger moved it (`IN_ROOM(ch) != was_in`). |
| Entry (mob), Enter (room) | `do_simple_move()` after the move | sends the mover back to the old room. |
| Greet, Greet-All | `do_simple_move()`, `spells.c` teleport/summon, `interpreter.c` login | sends the mover back (any greeter returning 0 is enough). |
| Door (mob, room) | `act.movement.c:do_doorcmd()` | blocks open/close/lock/unlock/pick. |
| Get | `act.item.c` get from room or container | blocks the get (also if the actor died or the object vanished). |
| Drop (obj, room) | `act.item.c:perform_drop()`, gold drops | blocks the drop. |
| Give (obj), Receive (mob) | `act.item.c:perform_give()`, `act.other.c:do_steal()` | blocks the transfer. |
| Wear, Remove | `act.item.c` wear/remove; zone reset `E`; equipment reload on save (only without `NO_EXTRANEOUS_TRIGGERS`, which is defined) | blocks it. `fight.c:make_corpse()` ignores the result. |
| Consume | `act.item.c` eat and drink, `spell_parser.c` quaff | blocks it. |
| Cast (room, obj, mob) | `spell_parser.c:call_magic()` | cancels the spell. |
| Death | `fight.c:raw_kill()` | suppresses the death cry. |
| Damage | `fight.c:damage()` | **replaces the damage**: the return value is the new damage. A damage trigger that does not `return %damage%` (or another number) reduces the hit to 1. |
| Login (room) | `interpreter.c:enter_player_game()` | ignored. |

### Who fires what

| Trigger | Fired from | Variables seeded | Notes |
| --- | --- | --- | --- |
| Random (all) | `dg_scripts.c:script_trigger_check()`, every `PULSE_DG_SCRIPT` (13 seconds) | none | Only if a player is in the zone (`is_empty()`), unless the script has Global. |
| Time (all) | `dg_scripts.c:check_time_triggers()`, every mud hour | `time` | `narg` is the hour; no chance roll. |
| Command | `command_mtrigger()` for mobs in the actor's room, `command_otrigger()` for the actor's equipment, inventory and room objects, `command_wtrigger()` for the room | `actor`, `cmd`, `arg` | The text argument is a command prefix (`en` matches `enter`) or `*`. Actors that fail `valid_dg_target(actor, 0)` (high immortals, nohassle) never fire command triggers. |
| Speech | `act.comm.c:do_say()` | `actor`, `speech` | See word matching below. The speaker's own mobile triggers fire only with `CONFIG_SCRIPT_PLAYERS`. |
| Act | `comm.c:perform_act()` for each NPC that receives an `act()` message | `actor`, `victim`, `object`, `target`, `arg` | `arg` is the message as that NPC saw it. |
| Greet, Greet-All, Memory | `do_simple_move()`, teleport/summon spells, login | `actor`, `direction` | Greet needs `CAN_SEE`; Greet-All does not. Memory runs `mremember` commands or Memory triggers. |
| Entry | `do_simple_move()` for the moving mob | none | |
| Leave | `do_simple_move()` | `actor`, `direction` | Mob leave needs `CAN_SEE`. |
| Door | `do_doorcmd()` | `actor`, `cmd`, `direction` | |
| Get, Drop, Give, Receive, Bribe, Wear, Remove, Consume | the `act.item.c` and `act.other.c` handlers | `actor`, and `object`, `victim`, `amount` or `command` as relevant | Bribe's `narg` is the minimum amount, not a chance. Wear, Remove and Consume have no chance roll. |
| Fight | `fight.c:hit()`, for the attacker, each attack | `actor` | |
| HitPrcnt | `fight.c:hit()`, for the victim | `actor` | `narg` is the hit-point percentage threshold. |
| Damage | `fight.c:damage()` | `actor`, `victim`, `damage`, `attacktype` | |
| Death | `fight.c:raw_kill()` | `actor` | `raw_kill()` sets the victim standing first so that ordinary commands work in the script, then extracts it. |
| Cast | `spell_parser.c:call_magic()` | `actor`, `spell`, `spellname` (room triggers also `victim`/`object`) | |
| Load | zone resets (`db.c:reset_zone()`), `mload`/`oload`/`wload`, `act.wizard.c:do_load()`, summoning and creation spells | none | See SCRIPT_ERROR_CODE below. |
| Timer | `limits.c:point_update()` when an object timer reaches 0 | none | All Timer triggers on the object run. |
| Zone Reset | end of `db.c:reset_zone()`, every room of the zone | none | |
| Enter | `do_simple_move()`, and every DG teleport (`dg_teleport_all()`, `mteleport`, `mgoto`, `oteleport`, `wteleport`), `goto`, `trans` | `actor`, `direction` (`none` for teleports) | |
| Login | `interpreter.c:enter_player_game()` | `actor` | |

The random and time checks walk `character_list` and test `WTRIG_RANDOM`,
`WTRIG_TIME` and `WTRIG_GLOBAL` against mobile scripts; the mobile bits happen
to have the same values. Objects are dispatched through
`dg_scripts.c:fire_object_triggers()`, which snapshots the candidates first.

### Word and phrase matching

Speech, Act and room Speech triggers match the spoken text against the text
argument:

* `narg` 0: **phrase mode.** `is_substring(arg, text)` must find the whole
  argument in the text, bounded by spaces, punctuation or the ends.
* `narg` non-zero: **word mode.** `word_check(text, arg)` splits the argument
  into words and `"quoted phrases"` (`one_phrase()`), and any one of them
  matching as above is enough.
* An argument of `*` matches anything (room Speech checks this explicitly;
  `word_check()` treats a leading `*` the same way).

Matching is case-insensitive (`str_str()`).

### Load triggers and SCRIPT_ERROR_CODE

`load_mtrigger()` and `load_otrigger()` treat a script result of
`SCRIPT_ERROR_CODE` (-9999999) as "this prototype loads things recursively":
they call `free_proto_script()` on the **prototype**, so no later copy gets
any triggers until the next reboot or OLC save. The value comes from
`script_driver()` when the nesting limit is hit, when the owner has no script,
or from a builder's `return -9999999`. Only the depth case is logged. Both
functions capture the prototype rnum before dispatch, because the script may
free its owner.

### Invariants

* A dispatcher never starts a trigger whose `depth` is non-zero.
* A dispatcher that continues after `script_driver()` must re-validate every
  pointer it uses (owner, actor, object, trigger). See
  [Callback safety](#callback-safety).
* Player-typed or player-spoken text enters a script only through
  `add_player_var()`.
* Handlers that index `world[IN_ROOM(actor)]` first call
  `dg_trigger_actor_placed()`, which reports a `trigger-dispatch` error and
  dispatches nothing for an actor outside a valid room.

## Script execution

### `script_driver()`: entry

```
int script_driver(void *go_adress, trig_data *trig, int type, int mode)
```

`go_adress` is a pointer to the caller's owner pointer (`&ch`, `&obj`,
`&room`). Passing the address lets the driver tell the caller that an object
owner was freed: it sets `*(obj_data **)go_adress = NULL`. `type` is the owner
kind and `mode` is `TRIG_NEW` (a fresh run from a dispatcher) or `TRIG_RESTART`
(resuming after a `wait`).

On entry the driver:

1. Resolves the owner and its script; a missing owner or script returns
   `SCRIPT_ERROR_CODE`.
2. Refuses to nest too deeply. A file-static `depth` counts active
   `script_driver()` frames across *all* triggers. When it already exceeds
   `MAX_SCRIPT_DEPTH` (10), the driver logs a `script-recursion` error,
   **extracts the owner's whole script**, and returns `SCRIPT_ERROR_CODE`. At
   most 11 frames can therefore be active at once.
3. Records a breadcrumb (`tb_breadcrumbf()`) naming the trigger, owner and
   depth, so any later `TB_ERROR_*` record says which script was running.
4. Snapshots the owner's identity: its UID, and for a mobile whether it was
   already pending extraction (`MOB_NOTDEADYET`).
5. Increments `depth` and calls `dg_script_execution_enter()`.
6. For `TRIG_NEW`: sets the trigger's `depth` to 1, its loop counter to 0, and
   the script's `context` to 0. A restart keeps them.

Every return path after step 5 decrements `depth` and calls
`dg_script_execution_leave()` exactly once.

### The line loop

The loop starts at `cmdlist` (new run) or `curr_state` (restart) and continues
while there is a line and the trigger's `depth` is non-zero. Each line is
classified in this order; the first match wins:

| Line starts with | Handling |
| --- | --- |
| `*` | Comment; skipped. |
| `if ` | Evaluate the condition; true opens a block (`depth++`), false jumps with `find_else_end()`. |
| `elseif `, `else` | Reached only after a true branch ran: jump to the matching `end` (`find_end()`) and close the block. At depth 1 it is reported and ignored. |
| `while ` | See [Control flow](#control-flow). |
| `switch ` | Jump to the matching `case`, `default` or `done` (`find_case()`). |
| `end` | Close an `if` block (`depth--`); at depth 1 reported and ignored. |
| `done` | Loop back to a `while`, or end a `switch`. |
| `break` | Jump to the next `done` (`find_done()`). |
| `case` | Nothing (allows fall-through between cases). |
| anything else | Expand variables with `var_subst()` into a `MAX_INPUT_LENGTH` buffer, then dispatch as a script command or a game command (next table). |

These keywords are recognized **by prefix and before variable expansion**:
`else` also matches `elsewhere`, `end` matches `endure`, `break` matches
`breakfast`. Control flow can never come from a variable. The script
commands below, by contrast, are matched *after* expansion, so a variable can
expand into one.

After expansion, a line is compared in this order (all are prefix matches
with the trailing space shown, except `halt`):

| Command | Implementation | Effect |
| --- | --- | --- |
| `eval <var> <expr>` | `process_eval()` | Evaluate an expression (expanding variables again) into a local. |
| `nop ...` | | Nothing; useful to expand a variable for its side effect. |
| `extract <var> <n> <text>` | `extract_value()` | The n-th word of the text. |
| `dg_letter <var> <n> <text>` | `dg_letter_value()` | The n-th character (deprecated; logs a deprecation note each time). |
| `makeuid <var> <id>` or `makeuid <var> mob\|obj\|room <name>` | `makeuid_var()` | Build a UID variable. A numeric id is evaluated as an expression. |
| `halt` | | End the run now. |
| `dg_cast '<spell>' [target]` | `dg_misc.c:do_dg_cast()` | Cast a spell (objects and rooms use a proxy caster). |
| `dg_affect <target> <property> <value> <duration>` | `dg_misc.c:do_dg_affect()` | Add (or with value `off`, remove) a `SPELL_DG_AFFECT` affect. |
| `global <var>` | `process_global()` | Move a local into the script's globals with the current context. |
| `context <n>` | `process_context()` | Set the script's current context. |
| `remote <var> <uid>` | `process_remote()` | Copy a local (or visible global) into another entity's globals. |
| `rdelete <var> <uid>` | `process_rdelete()` | Delete a global from another entity. |
| `return <n>` | `process_return()` | Set the run's return value. **Execution continues**; follow it with `halt` to stop. |
| `set <var> <text>` | `process_set()` | Set a local to literal (already expanded) text. |
| `unset <var>` | `process_unset()` | Remove a global, or if there is none, a local. |
| `wait ...` | `process_wait()` | Suspend; see [wait](#wait). |
| `attach`, `detach` | `process_attach()`, `process_detach()` | See [Attaching and detaching at run time](#attaching-and-detaching-at-run-time). |
| anything else | | A game command: `dg_restore_percent()` turns inert `%` back, then mobiles try `interpreter.c:script_command_interpreter()` (the `m*` table) and fall back to `command_interpreter()` as if the mob typed it; objects use `obj_command_interpreter()`; rooms use `wld_command_interpreter()`. |

After each of these, the driver checks that its own owner still exists (see
[Per-frame owner check](#per-frame-owner-check)). If not, it stops at once and
returns.

### Control flow

**`if` / `elseif` / `else` / `end`.** The trigger's `depth` is the current
block nesting plus one. A true `if` increments it. A false `if` calls
`find_else_end()`, which scans forward, skipping nested `if` blocks whole
(through `find_end()`), and stops at the first `elseif` whose condition is
true, an `else`, or the matching `end`; reaching a true `elseif` or an `else`
increments `depth` as a side effect. When execution later reaches an
`elseif`/`else` line, the true branch is finished, so the driver jumps to the
matching `end` and decrements `depth`. A missing `end` is logged and treated
as the end of the trigger.

**`while <cond>` / `done`.** On reaching `while`, the driver first finds the
matching `done` (`find_done()` skips nested `while`/`switch` blocks); if there
is none, the run ends with a `block-unterminated` error. If the condition is
true, the `done` line's `original` field is pointed at the `while` line and
the body runs; if false, execution continues after the `done`. On reaching
`done` with `original` set, the condition is evaluated again; true jumps back
to the `while` line (the loop then advances to its body). Two limits apply:

* every 30 iterations of the same loop (`cl->loops`), the driver inserts an
  implicit `wait 1` (one pulse) and returns, so a long loop cannot starve the
  game loop;
* when the run's total iterations (`GET_TRIG_LOOPS()`) reach 100, the run is
  stopped and "looped 100 times" is logged.

`cl->loops` and `original` live in the shared command list, so concurrent
instances of one trigger share the 30-iteration counter.

**`switch <expr>` / `case <value>` / `default` / `break` / `done`.**
`find_case()` evaluates the expression once and scans forward, skipping
nested blocks. It stops at the first `case` whose value compares equal
(`eval_op("==")` on a copy, because `eval_op()` trims in place and the line is
shared), **or at the first `default` it meets**, or at `done`. `default` must
therefore be the last label. Execution falls through `case` lines like C.
`break` jumps to the next `done`. An unterminated `switch` or `break` is
reported and ends the run.

### The expression evaluator

`if`, `elseif`, `while`, `switch`, `case`, `eval`, and the id arguments of
`makeuid`, `attach` and `detach` go through `eval_expr()`:

* `eval_lhs_op_rhs()` tokenizes the expression (parenthesized groups and
  quoted strings are single tokens; runs of letters, digits and spaces are
  single tokens) and splits at the **rightmost** top-level operator of the
  lowest precedence present, so operators of equal precedence associate to
  the left. Both sides are evaluated recursively.
* A whole expression in parentheses is unwrapped; anything else is a leaf and
  is expanded with `var_subst()`.
* Unary `!` is handled after the binary operators; a leading `+` or `-` (or
  one following another operator) is a sign, not an operator.

| Precedence (low to high) | Operators | Meaning |
| --- | --- | --- |
| 1 | `\|\|` | Logical or. Empty or `0` (first character) is false. |
| 2 | `&&` | Logical and. |
| 3 | `==` `!=` `<=` `>=` `<` `>` `/=` | Comparison. Numeric if both sides are integers (`is_num()`), otherwise case-insensitive string comparison. `/=` means "left contains right" (case-insensitive substring). |
| 4 | `+` `-` | Integer arithmetic. |
| 5 | `*` `/` | Integer arithmetic. |
| unary | `!` | Logical not (numeric or empty-string test). |

Arithmetic is done in `long long` and saturates to the `int` range; operands
outside `int` saturate too; division by zero yields 0. All three happen
silently in this branch. Results are written into `MAX_INPUT_LENGTH` buffers;
inputs are modified in place (operators are overwritten with NUL), which is
why `eval_expr()` works on copies.

Because leaves are expanded again, any text that reaches `eval` after a first
expansion is substituted twice. This is what makes the `%%name%%` idiom work,
and it is why player text must be stored inert (see
[Player text is never script syntax](#player-text-is-never-script-syntax)).

### wait

`process_wait()` accepts:

| Form | Delay |
| --- | --- |
| `wait <n>` | n pulses (`PASSES_PER_SEC` pulses per second, 10 by default) |
| `wait <n> s` (also `sec`, `secs`, `second`, `seconds`) | n seconds |
| `wait <n> t` | n mud hours |
| `wait until <HH:MM>` or `wait until <HHMM>` | until that time of the mud day; tomorrow if it has passed |

Any other suffix, a missing number, or trailing text is reported with
`script_log()` and **ends the run normally** (the trigger becomes idle again).
A delay longer than one mud year (`DG_MAX_WAIT_PULSES`) is logged and
shortened to a year. A zero or negative delay is clamped to one pulse by
`event_create()` without a message.

A valid wait allocates a `wait_event_data`, schedules
`trig_wait_event()` with `event_create()`, stores the event in
`GET_TRIG_WAIT(trig)`, sets `curr_state` to the next line, and returns from
the driver **without** freeing the locals or resetting `depth`. The trigger is
therefore busy (it will not fire again) until the rest of the script has run.

When the event fires, `trig_wait_event()` frees its payload, clears
`GET_TRIG_WAIT(trig)`, and checks that the owner pointer is still a live
character, object or room by scanning `character_list`, `object_list` or
`world[]` (comparing addresses, never dereferencing). If it is, it calls
`script_driver(&go, trig, type, TRIG_RESTART)`. If not, it logs "Trigger
restarted on unknown entity" and drops the continuation; the trigger keeps a
non-zero `depth` and will never fire again. Normally an owner cannot vanish
while its trigger waits, because extracting the owner extracts its script,
which cancels the wait (`extract_trigger()`). Rooms are the exception; see
[The event queue](#the-event-queue).

### Ending a run

| How the run ends | Locals | `depth` | Owner pointer |
| --- | --- | --- | --- |
| Last line, `halt`, loop limit, invalid `wait`, unterminated block | freed | 0 (idle) | unchanged |
| Valid `wait` (or the implicit one in a long loop) | kept | kept (busy) | unchanged |
| Owner freed or extracted by a command in this frame | kept (freed later with the trigger) | kept | objects: caller's pointer set to NULL |
| Nesting limit | the owner's script is extracted | | |

The locals are freed even when the trigger was detached during the run; the
detached trigger itself is freed later (see [Callback safety](#callback-safety)).

### Invariants

* `script_driver()` keeps `depth` and `dg_execution_depth` balanced on every
  return path.
* A caller must not use its owner pointer after `script_driver()` returns
  without checking it: for objects use the UID (`obj_uid_matches()`) or the
  NULLed pointer; for mobiles check pending extraction.
* Command buffers are `MAX_INPUT_LENGTH`; `var_subst()` output is bounded to
  that size.
* A trigger is busy (`depth != 0`) from the start of a run until it finishes,
  including any time spent waiting.

## Variables and substitution

### Where variables live

| Kind | Stored in | Created by | Lifetime |
| --- | --- | --- | --- |
| Local | `trig->var_list` | dispatchers (`actor`, `arg`, ...), `set`, `eval`, `extract`, `makeuid`, `dg_letter`, field chains (`tmpvr`) | until the run ends |
| Global | `sc->global_vars` | `global`, `remote` from another script, `mload`/`oload`/`wload` (`lastloaded`), immortal `set <char> <var> <value>` (`perform_set_dg_var()`), saved player variables | as long as the script; for players, saved in the player file |
| "Full global" | the globals of room 0 (the Void) | `remote <var> %global%` (`%global%` expands to room 0's UID) | as long as room 0's script; read with `%global.<name>%` |

`add_var()` copies the name and value. A name containing `.` is refused (it
would be read back as a field access); the refusal is only logged.

**Contexts.** A global carries a `context` number. A run sees globals whose
context is 0 or equal to `sc->context`, which a script sets with
`context <n>` (often a player's idnum, to keep per-player state on one mob).
`global` stores with the current context. `add_var()` updates an existing
variable of the same name only if its context is 0 or the same; otherwise it
adds a new one in front. As a result, a context-0 global with the same name
absorbs every context-specific `global` of that name. Each new run starts with
context 0.

### `var_subst()`: the `%...%` syntax

`var_subst(go, sc, trig, type, line, buf)` copies `line` into `buf`
(`MAX_INPUT_LENGTH`), replacing each reference:

| Syntax | Meaning |
| --- | --- |
| `%%` | A literal `%`. |
| `%name%` | The variable's value, or a special name (below). |
| `%name.field%` | A field of the entity (or text) the variable names. |
| `%name.field(sub)%` | A field with an argument; the argument is expanded first, recursively. |
| `%a.b.c%` | A chain: each field's value is bound to a temporary local `tmpvr` and the next field is applied to it. |

The output of `var_subst()` is not scanned again. A reference that runs to
the end of the line (no closing `%`, or an unclosed `(`) is substituted as
read and reported (`var-unterminated`). The value bound to `tmpvr` is stored
as-is and never evaluated, so field text (an object name, a title) cannot
inject operators or variables.

### `find_replacement()`: resolution order

For a reference `var.field(sub)`:

1. Look for a local named `var`, then a global visible in the current context.
2. With no field: the variable's value. Without a variable, special names:
   `self` (the owner's UID), `global` (room 0's UID), and the owner-kind
   command aliases `%echo%`, `%send%`, `%echoaround%`, `%force%`, `%load%`,
   `%purge%`, `%teleport%`, `%damage%`, `%door%`, `%zoneecho%`, `%asound%`,
   `%at%`, `%transform%`, `%recho%`, `%move%`, `%log%`, which expand to
   `mecho`/`oecho`/`wecho` and so on. These let one script text work for any
   owner kind. (`%transform%` in a room and `%move%` in a mob expand to the
   echo command, which have no room or mobile counterpart.)
3. With a field and a variable: first the string fields of
   `text_processed()` (`strlen`, `toupper`, `trim`, `contains`, `car`, `cdr`,
   `charat`, `mudcommand`). Otherwise the value is treated as an entity name or
   UID and resolved, depending on the owner kind, through `get_char_*()`,
   `get_obj_*()` and `get_room()` (see
   [Unique IDs and the lookup table](#unique-ids-and-the-lookup-table)).
4. With a field and no variable: `self`, `global.<name>`, `people.<vnum>`,
   `time.hour|day|month|year`, `happyhour.*`, `random.char`, `random.dir`,
   `random.<n>`, `findmob.<room>(<vnum>)`, `findobj.<room>(<vnum|name|uid>)`.
5. On the resolved character, object or room, a field switch
   (`%actor.name%`, `%obj.vnum%`, `%room.north(vnum)%`, ...). An unknown field
   falls back to that entity's global variable of the same name, then to a
   logged "unknown field".

### Fields that change the game

Many fields are setters when given an argument. Expanding a variable can
therefore change game state. For characters this includes `gold`, `exp`
(via `gain_exp()`), `hitp`, `mana`, `move`, their maxima, `hitroll`,
`damroll`, `str`/`stradd`/`int`/`wis`/`dex`/`con`/`cha`, `align`, `class`,
`level` (clamped below `LVL_IMMORT`), `pos`, `practices`, `questpoints`,
`saving_*`, `hunger`/`thirst`/`drunk`, `skillset`, `is_killer`, `is_thief`,
`title`, `wait`, and others; for objects `cost`, `cost_per_day`, `weight` and
`oset` (alias, apply, short and long descriptions through `genobj.c`). Only
`title` checks `valid_dg_target()`. The arithmetic setters add a script `int`
to the field and store the result, so a field narrower than `int` (hit
points are `sh_int`) wraps on overflow in this branch.

### Player text is never script syntax

Player text reaches scripts as `%arg%`, `%speech%`, `%cmd%`, act messages and
player-set fields such as titles. Because some lines are substituted twice
(see the evaluator), a `%` typed by a player could otherwise become live
syntax such as `%actor.level(34)%`. The rule:

* store player text with `add_player_var()` or `dg_neutralize_percent()`,
  which turn every `%` into the inert byte `DG_INERT_PERCENT` (`\x1a`);
  `comm.c` accepts only printable ASCII, so a player cannot type it;
* `var_subst()` never treats the inert byte as syntax;
* `dg_restore_percent()` turns it back where text leaves the engine: game
  commands just before dispatch, both operands of `eval_op()` (whose results
  are numbers), stat listings (`dg_display_value()`), and a title set by a
  script. Never restore it where the text could be substituted again.

### Saved player variables

Players' globals are saved in the player file as a `Vars: <count>` block
(`dg_scripts.c:save_char_vars_ascii()`) and read back by
`read_saved_vars_ascii()`. Each line is `name context value`. Variables whose
name starts with `-` are session-only and never saved. A variable whose line
would not fit `READ_SIZE` or whose value contains a line break is not saved,
and a `var-save` error is reported. The legacy per-player file is read by
`read_saved_vars()` at login only when the player file had no block, and
deleted with the character (`dg_handler.c:delete_variables()`).
`read_saved_vars()` creates `SCRIPT(ch)` whether or not a file exists, so every
player in the game has a script structure (normally with no triggers).

### Invariants

* `var_subst()` and every result buffer hold `MAX_INPUT_LENGTH` bytes; the
  parameters carry no size, so callers must pass buffers that large.
* Expanding a variable may mutate game state; never expand a script line
  "just to look".
* `find_replacement()` may be called with `trig == NULL` (the `.global`
  path); it handles that.
* The value of `tmpvr` and of any field is text, never evaluated.

## Unique IDs and the lookup table

### Namespaces

A UID is a `long`. Scripts write it with the marker `}` (`UID_CHAR`), as in
`}10000042`. The value tells you the kind:

| Range | Kind | Assigned by |
| --- | --- | --- |
| `[0, MOB_ID_BASE)` (below 10,000,000) | Player | the player's idnum, registered at login by `interpreter.c:enter_player_game()` |
| `[MOB_ID_BASE, ROOM_ID_BASE)` | Mobile | `dg_scripts.c:char_script_id()`, on first use |
| `[ROOM_ID_BASE, OBJ_ID_BASE)` | Room | computed: `ROOM_ID_BASE + vnum` (`room_script_id()`); never registered |
| `[OBJ_ID_BASE, LONG_MAX)` | Object | `dg_scripts.c:obj_script_id()`, on first use |

`find_char()` accepts only player and mobile UIDs, `find_obj()` only object
UIDs, `find_room()` only room UIDs whose vnum part fits a vnum. Always fetch an
ID through `char_script_id()` or `obj_script_id()`, never by reading
`script_id` directly: the field is 0 until the first fetch, which allocates and
registers it.

### Allocation

Mobile IDs come from `max_mob_id` and stay inside the mobile range: on
leaving the range the counter wraps (with a `uid-allocate` warning and a
builder mudlog) and skips IDs still registered. If every mobile ID is in use
the server logs a fatal error and aborts. Object IDs grow from `max_obj_id`
and wrap the same way at `LONG_MAX`. IDs are therefore reused only after a
wrap, and never while still registered.

### The lookup table

`find_char()` and `find_obj()` resolve UIDs through a 64-bucket hash table
(`lookup_table[]`, keyed by `uid & 63`). Each bucket has a sentinel head
(`uid = UID_OUT_OF_RANGE`, 1,000,000,000) and a chain of `{uid, pointer}`
entries.

* `add_to_lookup_table(uid, ptr)`: adds an entry, or **rebinds** an existing
  one to the new pointer with a `uid-rebind` warning. Rebinding is expected
  when a player logs in while an older character with the same idnum still
  awaits extraction.
* `remove_from_lookup_table(uid, ptr)`: removes the entry **only if it still
  maps to `ptr`**. Freeing that older character must not remove the live
  player's mapping, so a mismatch is reported (`uid-remove` warning) and the
  mapping kept. A missing entry is reported as an error. UID 0 is ignored.
* Entries are removed by `db.c:free_char()` and `db.c:free_obj()`, that is,
  when the memory is actually freed. A character pending extraction is still
  registered.
* `obj_uid_matches(uid, obj)` checks that `uid` is an object UID and that its
  entry points at `obj`, **comparing the address without dereferencing it**.
  This is the safe way to ask "is this object still the one I saw?" after a
  callback that may have freed it.

### Resolving names and UIDs

Script commands accept either a UID or a name. The resolvers in
`dg_scripts.c` all take a leading `}` as a UID, and otherwise search by
keyword in a scope that depends on the caller:

| Resolver | Name search scope | UID path |
| --- | --- | --- |
| `get_char()` | whole world | `find_char()` |
| `get_char_in_room()`, `get_char_near_obj()` | one room | `find_char()`, **anywhere** |
| `get_char_by_room()`, `get_char_by_obj()` | the room (or holder), then the world | `find_char()` |
| `get_obj()` | whole world | `find_obj()` |
| `get_obj_in_room()` | room contents | scans the room |
| `get_obj_near_obj()`, `get_obj_by_obj()` | contents, container, holder, room; `by_obj` then the world | `by_obj`: `find_obj()`; `near_obj`: only the containing object |
| `get_obj_by_room()` | the room, then the world | `find_obj()` |
| `get_room()` | a vnum (`dg_parse_vnum()`) | `find_room()` |

Character resolvers filter with `valid_dg_target(ch, DG_ALLOW_GODS)`. A UID
lookup ignores the locality a name lookup would apply (several functions carry
a `@todo` about this). A pointer returned by a resolver is valid only until
the next callback.

### Invariants

* A character or object pointer obtained from a UID is as live as its lookup
  entry: the entry disappears only when the memory is freed.
* Room UIDs are not in the table; they are recomputed from the vnum and
  survive room renumbering.
* Mobile UIDs never leave the mobile range; object UIDs never fall below
  `OBJ_ID_BASE`.

## Callback safety

The full rules, with the helper names, are in section 2 of
[../maintainer-safety-guide.md](../maintainer-safety-guide.md). This section
explains why they hold in the engine.

### What a callback can do

Running a trigger can purge, teleport or kill the owner, the actor or anyone
else; detach or attach any trigger, including the one running; load objects
and mobiles; force characters to type commands (which fire *their* command
triggers); and start nested scripts up to the depth limit. Any code that keeps
working after calling `script_driver()`, a dispatcher, `command_interpreter()`
or any game function that might reach them must assume all of that happened.

The lifetimes differ by kind:

| Entity | When it is freed | What a stale pointer looks like |
| --- | --- | --- |
| Trigger, script | retired, freed when the outermost script frame returns | allocated but off every list; `serial` identifies it |
| Character | `extract_char()` marks it (`MOB_NOTDEADYET`/`PLR_NOTDEADYET`); freed at the end of the pulse by `extract_pending_chars()` | allocated, still on its lists, flagged; may have moved |
| Object | `extract_obj()` frees it **immediately**, contents first | freed memory; check with `obj_uid_matches()` before touching it |
| Room | never freed by a script | always valid, but `world[]` can move when OLC adds or deletes a room |

See [gameplay.md](gameplay.md#character-and-object-lifetime) for
`extract_char()` and `extract_obj()` themselves.

### Retirement: deferred freeing of triggers and scripts

`dg_handler.c` keeps a counter, `dg_execution_depth`, incremented by
`dg_script_execution_enter()` and decremented by
`dg_script_execution_leave()` around every `script_driver()` frame.

* `extract_trigger(trig)` cancels the trigger's wait, decrements the
  prototype's live count, and unlinks it from `trigger_list`. If any script is
  running it pushes the trigger on a retired list; otherwise it frees it. The
  caller must already have unlinked it from the owner's `trig_list`
  (`remove_trigger()` does; `extract_script()` clears the whole list).
* `extract_script(thing, type)` detaches the script from its owner
  (`SCRIPT(thing) = NULL`), extracts every trigger, and retires or frees the
  `script_data` and its globals. It asserts that no other character, object
  or room still points at the script.
* When the counter returns to 0, `dg_script_execution_leave()` frees all
  retired triggers (`free_trigger()`: name, argument, locals, and its wait if
  one was scheduled meanwhile) and then all retired scripts (their globals and
  the struct).

So a frame whose trigger or script is detached mid-run keeps running on
allocated memory: its command list still belongs to the prototype, its locals
are freed normally at the end of the run, and a `wait` it schedules is
cancelled when the retired trigger is freed. `free_trigger()` never frees the
command list.

### Snapshot, then re-validate

Dispatchers and commands that act on several entities, or that continue
after a callback, never follow a `next` pointer saved before the callback.
They snapshot identities first and re-acquire each entity from a live list:

| What is iterated | Snapshot | Re-validation | Examples |
| --- | --- | --- | --- |
| Characters in a room | UIDs (`dg_memory_targets()`) | `find_char(uid)` plus `dg_memory_character_live()`: same pointer, not pending extraction, still in the room | `greet_mtrigger()`, `greet_memory_mtrigger()`, `entry_memory_mtrigger()`, `command_mtrigger()`, `speech_mtrigger()`, `dg_teleport_all()` |
| An owner's triggers | (address, serial) pairs (`dg_greet_trigger_targets()`) | search the live `trig_list` with `dg_trigger_target_is()`; a matching address with a different serial is a reused allocation, skipped with a `trigger-identity` warning | `greet_mtrigger()`, `command_mtrigger()`, `cmd_otrig()`, `timer_otrigger()`, `leave_otrigger()` |
| Objects | (UID, address) pairs (`struct dg_cmd_object_target`, `struct dg_object_target`) | `obj_uid_matches()`, then confirm the object is still where it was (equipment slot, inventory, room) | `command_otrigger()`, `fire_object_triggers()`, `leave_otrigger()` |
| A mob's DG memory | count of matching nodes on entry | search the live list again after each callback; detach the node before running its script; copy a remembered command before running it | `greet_memory_mtrigger()`, `entry_memory_mtrigger()` |

The bound on memory nodes stops a callback that keeps re-remembering the actor
from looping forever.

After a single dispatch, gameplay callers re-check what they need:
`get_otrigger()`, `drop_otrigger()`, `give_otrigger()`, `wear_otrigger()`,
`remove_otrigger()`, `consume_otrigger()`, `receive_mtrigger()` and
`drop_wtrigger()` return 0 when the object no longer matches its UID (or is
no longer carried by the actor, or the actor died), so the caller does not
complete the action on a freed object. `do_simple_move()` checks
`IN_ROOM(ch) != was_in` after each leave trigger.

### Per-frame owner check

After every non-control line, `script_driver()` calls
`dg_frame_owner_gone()`:

* **Object owner:** gone if `obj_uid_matches(owner_uid, go)` fails. The
  driver sets the caller's object pointer to NULL and returns. This catches
  any way the object was freed: `opurge self`, purging its container, a
  forced `junk`, a nested script.
* **Mobile owner:** gone if it was not pending extraction when the frame
  started and is now (`MOB_NOTDEADYET`). The driver returns; the caller's
  pointer stays valid until the end of the pulse.
* **Room owner:** never gone.

Each frame checks only its own owner, so nested scripts cannot hide or fake
the result. Control-flow lines are not followed by the check; they only
evaluate expressions, which can change fields but not free entities.

Two consequences are worth knowing. A script can run on behalf of a mobile
that is already pending extraction (for example a `wait` that expires in the
pulse in which a player killed the mob); the check only notices extraction
caused during the frame. And the check uses `MOB_FLAGGED()`, which is false
for players, so a trigger on a player (`CONFIG_SCRIPT_PLAYERS`) keeps running
after its owner is extracted.

### Recursion and loop limits

| Limit | Where | Value | On overflow |
| --- | --- | --- | --- |
| Nested `script_driver()` frames | `script_driver()` (`MAX_SCRIPT_DEPTH`) | 10 (11 frames) | error logged, owner's script extracted, `SCRIPT_ERROR_CODE` returned (load triggers then strip the prototype's trigger list) |
| Re-entry of one trigger | `TRIGGER_CHECK()` | a busy trigger never starts | silently skipped |
| Iterations of one `while` before yielding | `script_driver()` | 30 | implicit `wait 1` |
| Loop iterations per run | `script_driver()` | 100 | run stopped, logged |
| `wait` length | `process_wait()` | one mud year | shortened, logged |
| Line length after expansion | `var_subst()` | `MAX_INPUT_LENGTH` (512) | truncated |
| Memory callbacks per greet | `greet_memory_mtrigger()` | nodes present on entry | later nodes wait for the next greet |

### Invariants

* Never follow `t->next`, `obj->next_content`, `ch->next_in_room` or
  `ch->next` saved from before a callback (walking `character_list` is safe
  only because extraction is deferred).
* Compare a possibly freed object with `obj_uid_matches()`, never by reading
  its fields.
* Compare a possibly freed trigger by address and serial against a live list.
* Capture anything needed after a dispatch (a prototype rnum, a vnum for a
  log line) before dispatching.

## The event queue

`dg_event.c` implements one global priority queue of `struct event`, used by
DG `wait` and by the typed mud events (`mud_event.c`, described in
[gameplay.md](gameplay.md#events-and-generic-lists)).

### Structure

```
event_q (struct dg_queue)
  head[0..9], tail[0..9]       bucket = key % NUM_EVENT_QUEUES
     |
     v
  q_element {data -> struct event, key = absolute pulse, prev, next}
     each bucket sorted by key

struct event {func, event_obj, q_el, isMudEvent}
```

The key is the absolute pulse at which the event is due
(`when + pulse`, clamped by `event_key()` so it can never wrap negative). An
event due at pulse P is in bucket `P % 10`, so `event_process()` only looks at
one bucket per pulse: it runs every event at the head of the current bucket
whose key has arrived. Events due on the same pulse run in reverse order of
creation (`queue_enq()` inserts in front of equal keys).

### Functions and ownership

| Function | Contract |
| --- | --- |
| `event_init()` | Creates `event_q`; must run before the first `event_create()`. |
| `event_create(func, obj, when)` | Allocates and enqueues an event `when` pulses from now (`when < 1` becomes 1 without a message). The event owns `obj`. |
| `event_process()` | Called first in every `comm.c:heartbeat()`. For each due event: sets `q_el = NULL` (the "running" marker), calls `func(obj)`; a positive return re-enqueues it that many pulses later; otherwise frees a mud event's payload with `free_mud_event()` and then the event. For a DG wait the callback has already freed its own payload. |
| `event_cancel(ev)` | Dequeues the event, frees its payload (`cleanup_event_obj()`: `free_mud_event()` or `free()`), and frees it. The caller must clear every pointer it holds to the event (`GET_TRIG_WAIT()`, owner event lists). |
| `event_time(ev)` | Pulses until the event fires (shown for waiting triggers by the `stat` script listing, `dg_scripts.c:script_stat()`). |
| `event_free_all()` | Frees the queue and every queued event and payload at shutdown. It leaves `event_q` pointing at freed memory. |

**Payload rule.** A callback that returns a positive value must not free its
payload, because the event runs again with it. A DG callback that returns 0
must free it itself (`trig_wait_event()` does so first thing). Mud event
payloads are always freed by the engine.

**Do not cancel a running event.** While `func` runs, its event has
`q_el == NULL`. If anything reached from the callback calls `event_cancel()`
on that same event, the event is freed while `event_process()` still holds it
and then uses it again (re-enqueue or a second free). `event_cancel()` only
logs "Attempted to cancel a non-NULL unqueued event, freeing anyway". DG waits
avoid this: `trig_wait_event()` clears `GET_TRIG_WAIT(trig)` before running
the script, so a nested `extract_trigger()` has nothing to cancel. Mud events
do not have that protection (see
[gameplay.md](gameplay.md#events-and-generic-lists)).

### How `wait` uses the queue

`process_wait()` creates the event with a `wait_event_data` payload holding
the trigger, the owner pointer and the owner type; `trig_wait_event()` is the
callback and always returns 0. `extract_trigger()` and `free_trigger()` cancel
a trigger's pending wait, so an owner that is extracted takes its waits with
it.

Room owners are the weak spot. The payload stores `&world[rnum]`. When OLC
adds or deletes a room, `genwld.c:add_room()` and `genwld.c:delete_room()`
`RECREATE()` the `world` array, and `dg_handler.c:update_wait_events()` repoints
only the rooms they shift. If `realloc` moved the array, every other room's
pending wait still points into the old block. `trig_wait_event()` then fails
to find the pointer in `world[]`, logs "Trigger restarted on unknown entity",
and drops the rest of the script; the trigger stays busy and never fires again
until the room's script is rebuilt. Nothing is dereferenced, so this is a
behaviour fault, not a memory fault.

### Invariants

* Keys are non-negative; a bucket index is always `key % 10` of a
  non-negative key.
* An event is either queued (`q_el != NULL`) or running (`q_el == NULL`, owned
  by `event_process()`); it must not be cancelled while running.
* `GET_TRIG_WAIT(t)` is non-NULL exactly while a wait for `t` is queued.
* After `event_free_all()` no event function may be called.

## Command sets

### How a command reaches its implementation

* **Mobiles:** `script_driver()` first tries
  `interpreter.c:script_command_interpreter()`, which looks the first word up
  in `mob_script_commands[]` (exact match only). The `m*` commands are not in
  the player command table, so a player cannot type them; an implementor
  switched into a mob can (`command_interpreter()` calls the same function for
  that case). Anything else goes through `command_interpreter()` exactly as if
  the mob typed it, including command triggers of others in the room.
* **Objects and rooms:** `obj_command_interpreter()` and
  `wld_command_interpreter()` look the first word up by prefix in
  `obj_cmd_info[]` and `wld_cmd_info[]`, in table order, so an abbreviation
  runs the first entry it abbreviates (`o` runs `oasound`, `w` runs
  `wasound`). An unknown word is logged. The first entry, `RESERVED`, has no
  function; `any_one_arg()` lowercases the word, so it cannot match.

Each `m*` command first checks `MOB_OR_IMPL(ch)`: an NPC that is not switched
into (or is switched into by an implementor), **or any character whose script
has triggers**. Charmed mobs silently do nothing. `mload`, `mpurge`,
`mforce`, `mhunt`, `mremember` and `mforget` additionally refuse a character
with a descriptor unless `GET_LEVEL(ch->desc->original)` is at least
`LVL_IMPL`; `mtransform` refuses any character with a descriptor.

### The three sets side by side

| Purpose | Mobile | Object | Room | Notes |
| --- | --- | --- | --- | --- |
| Message the room | `mecho` | `oecho` | `wecho` | `oecho`/`mecho` use `sub_write()`; `wecho` uses `act()` through the first occupant (`act_to_room()`), so `$n` means that occupant. |
| Message one character | `msend` | `osend` | `wsend` | |
| Message everyone but one | `mechoaround` | `oechoaround` | `wechoaround` | |
| Message adjacent rooms | `masound` | `oasound` | `wasound` | `masound` temporarily sets the mob's `in_room` to each neighbour without moving it. |
| Message a zone | `mzoneecho` | `ozoneecho` | `wzoneecho` | `send_to_zone()`: awake players in the zone containing the given room vnum. |
| Message a vnum range | `mrecho` | `orecho` | `wrecho` | `send_to_range()`. |
| Edit an exit | `mdoor` | `odoor` | `wdoor` | Any room in the world; fields `purge`, `description`, `flags`, `key`, `name`, `room`. |
| Force a command | `mforce` | `oforce` | `wforce` | Target must pass `valid_dg_target(ch, 0)`; `mforce all` also requires lower level and `CAN_SEE`. |
| Load | `mload` | `oload` | `wload` | `mob <vnum> [room]` or `obj <vnum> [char\|container [wear position]]`; sets the global `lastloaded`; fires load triggers. |
| Purge | `mpurge` | `opurge` | `wpurge` | No argument: every NPC and object in the room (the mob and the object exclude themselves). Never players. |
| Teleport | `mteleport` | `oteleport` | `wteleport` | `all` uses `dg_teleport_all()`; fires enter triggers. |
| Damage | `mdamage` | `odamage` | `wdamage` | `script_damage()`. |
| Run a command elsewhere | `mat` | `oat` | `wat` | See below. |
| Transform | `mtransform` | `otransform` | | See below. |
| Move objects | | `omove` (itself) | `wmove` (an object or `all`) | |
| Timer / values | | `otimer`, `osetval` | | |
| Log to builders | `mlog` | `olog` | `wlog` | `script_log()` with the owner named; text passed as data, never as a format. |
| Mobile only | `mkill`, `mjunk`, `mgoto`, `mhunt`, `mremember`, `mforget`, `mfollow` | | | |

Notes on the commands with surprising semantics:

* **Teleport targets.** `mteleport` and `mgoto` use
  `act.wizard.c:find_target_room()`; `oteleport` and `omove` use
  `dg_objcmd.c:find_obj_target_room()`, which refuses god rooms, houses and
  occupied private rooms; `wteleport` and `wmove` accept any room vnum.
* **`mat`** moves the mob to the room, runs the command, and moves it back if
  it is still there. **`oat`** creates a temporary copy of the object (a fresh
  instance with the prototype's triggers and no variables), puts it in the
  room, runs the command through the copy, and extracts the copy if its UID
  still matches. **`wat`** runs the command as the target room.
* **`mtransform <vnum>`** (or `-<vnum>` to take the new hit points) loads the
  new mobile, struct-copies it over the old one, and puts back a chosen set of
  the old fields: script and `proto_script`, UID, affects, inventory and
  equipment, DG memory, events, list links, followers, group, furniture link,
  position, fighting and hunting, and the old rnum (so instance counts stay
  right). The old mob's scripts survive; the new prototype's triggers are
  extracted with the temporary mob.
* **`otransform <vnum>`** does the same for objects, keeping location,
  contents, UID, scripts, sitters and events, but taking the new rnum.
* **`dg_cast`** is cast by the mobile itself at its level. Objects and rooms
  use a proxy: a fresh copy of mobile `DG_CASTER_PROXY` (vnum 1), linked into
  the room by hand (not with `char_to_room()`), named after the object or
  "The gods", casting at `DG_SPELL_LEVEL` (25), then extracted (deferred).
  Group spells are refused; a target must exist unless the spell takes none.
* **`dg_affect`** applies an `APPLY_*` modifier or sets an `AFF_*` bit as
  `SPELL_DG_AFFECT` with the given duration; value `off` removes every
  `SPELL_DG_AFFECT` affect from the target.
* **`script_damage()`** subtracts the amount from hit points (negative amounts
  heal, capped at the maximum), updates the position, and calls `die()` at
  `POS_DEAD`, logging script deaths of players. Immortals ignore positive
  damage.

### Message helpers

`dg_comm.c:sub_write(arg, ch, find_invis, targets)` expands tokens for each
recipient separately: `~name` (the character's name, or "you"), `|name`
(possessive), `^name` (his/her/its), `&name` (he/she/it), `*name`
(him/her/it) and `` `name `` (an object). `\` escapes the next character.
`name` may be a UID. Lookups use `ch`'s room as the anchor; with
`find_invis` (every DG caller passes TRUE) visibility is ignored. The
expansion is bounded and reports truncation. It runs no callbacks, so walking
the room's occupant list inside it is safe.

### Invariants

* `obj_room()` returns `NOWHERE` for an object nowhere in the world; check it
  before indexing `world[]`.
* Commands that `extract_obj()` their own owner (directly or through a
  container) must not touch the owner afterwards; `script_driver()` stops the
  frame on return.
* A command that runs `command_interpreter()` for several characters must
  re-acquire each one after the previous command (see the snapshot rules).

## What a builder-written script can and cannot do

A trigger is ordinary builder data, but it runs with considerable power. This
list is for maintainers deciding whether a change widens or narrows it.

**A script can:**

* send any text to a room, a character, a zone or a range of rooms;
* load any mobile or object by vnum anywhere a room vnum can name;
* purge any NPC or object it can name (by UID: anywhere in the world);
* teleport any character it can name, subject to `valid_dg_target()` and the
  per-command room rules above;
* force characters to type commands (except those `valid_dg_target(ch, 0)`
  protects), which can make players drop, give, wear or say anything;
* damage or heal characters, including killing players;
* cast any non-group spell, add any affect;
* change most character and object fields through variable setters,
  including gold, experience, ability scores, skills, flags, title and level
  (below immortal);
* edit any exit of any room;
* read and write global variables of any entity with a script, including
  players' saved variables (`remote`, `rdelete`);
* attach and detach triggers on any mobile, object or room by UID (players
  only with `CONFIG_SCRIPT_PLAYERS`);
* run for as long as it likes in wall-clock time by waiting (up to a year per
  wait).

**A script cannot:**

* purge a player (`mpurge`, `opurge` and `wpurge` refuse);
* target an immortal with no-hassle on, or force a `LVL_GRGOD` and above
  (`valid_dg_target()`); immortals take no positive script damage;
* raise a character's level to immortal (`level` is clamped to
  `LVL_IMMORT - 1`);
* trigger command triggers as a protected immortal (those actors are skipped);
* nest scripts more than 11 frames deep, loop more than 100 times per run, or
  run a line longer than `MAX_INPUT_LENGTH`;
* create control flow from variables (keywords are matched before
  expansion);
* make player text into script syntax (the inert `%` rule);
* use `m*` commands from a player's keyboard.

**Where the limits are weaker than they look** (behaviour of this branch):

* the `level` setter has no target check, so it can *lower* an immortal's
  level to `LVL_IMMORT - 1`;
* `mdoor`, `odoor` and `wdoor` edit exits anywhere, regardless of zone;
* `wteleport` and `wmove` ignore god rooms, houses and private rooms;
* the immortal `vdelete` command (level `LVL_BUILDER`) can delete any
  entity's globals, including a player's saved variables, with no zone check.

## Trigger OLC

### `trigedit`: lifecycle

`trigedit <vnum>` (`dg_olc.c:do_oasis_trigedit()`, `LVL_BUILDER`) refuses a
vnum outside the index type, a vnum already open in another `CON_TRIGEDIT`
session, a vnum with no zone, and a zone the builder may not edit (see
[olc.md](olc.md#builder-permissions)). It creates the descriptor's OLC data
(see [olc.md](olc.md#descriptor-olc-state)) and builds a scratch trigger:

* `trigedit_setup_new()`: `CREATE()`s a trigger with `nr = NOWHERE`, a default
  name and type (Greet, `narg` 100), and a `MAX_CMD_LENGTH` text buffer in
  `OLC_STORAGE(d)`.
* `trigedit_setup_existing(d, rnum)` (also used by `tcopy`): copies the
  prototype with `trig_data_copy()` (so the scratch trigger **borrows the
  prototype's `cmdlist`** and copies its `nr`) and renders the command lines
  into `OLC_STORAGE(d)`, sized to fit the script but at least
  `MAX_CMD_LENGTH`.

`trigedit_parse()` drives the menu: name, attach type, type bits, numeric
argument (clamped to 0..100), text argument, and the commands, which are
edited as text in `OLC_STORAGE` with the string editor (`max_str` is
`MAX_CMD_LENGTH`; the `/f` formatter is `format_script()`). `W` copies another
trigger's contents into the draft but keeps the draft's own rnum.

`cleanup_olc()` frees `OLC_STORAGE` and the scratch trigger with
`free_trigger()`, which leaves the borrowed command list alone.

### `trigedit_save()`

**Existing trigger.** The prototype's command list is freed, a new one is
built from `OLC_STORAGE` with `strtok()` (an empty script becomes the single
line `* No script`), the scratch trigger is copied into the prototype
(`trig_data_copy()`, which gives the prototype a new serial), and the
prototype keeps its rnum. Then every live trigger with that rnum on
`trigger_list` is updated in place: new name and argument copies, its pending
wait cancelled, its locals freed, `cmdlist` and `curr_state` pointed at the new
list, type, attach type and `narg` copied, `depth` reset to 0. Any run in
progress is abandoned. This is safe only because OLC input is never processed
while a script runs, so no retired trigger can still hold the old list.

**New trigger.** A new `trig_index` array one larger is built with the new
prototype inserted at its sorted position. Every prototype above it gets
`nr + 1`, and so does every live trigger and every other `CON_TRIGEDIT`
session's scratch trigger whose rnum is at or above the insertion point.
Nothing renumbers the trigger rnums held by `'T'` zone reset commands, live
or in `zedit` sessions: after an insert, every `'T'` command for a higher
trigger vnum names the trigger just below it, so resets attach the wrong
trigger and `save_zone()` writes the wrong vnum. See
[olc.md](olc.md#renumbering-on-insert-and-delete) for the general renumbering
pattern.

**Disk write.** The in-memory changes above are made first and are not rolled
back. The zone's triggers are then checked against the size `fread_string()`
can read back; if one is too large the save stops with a `LVL_GOD` mudlog and
the zone file is not written. Otherwise `lib/world/trg/<zone>.new` is written,
flushed and closed with checks, then renamed over `<zone>.trg`
(`tb_replace_file_checked()`), and `trigedit_create_index()` adds the zone to
`lib/world/trg/index` if it is missing. Failures after the in-memory save are
reported only through mudlogs and the server log; the builder sees "Trigger
saved to disk." only on success, while the caller logs "OLC: <name> edits
trigger <n>" either way.

### The attached-trigger editor in `medit`, `oedit` and `redit`

The `S` (script) option of the mobile, object and room editors edits the
draft's list of trigger vnums. The ownership model (explained in a comment in
`dg_olc.c:dg_script_edit_parse()`):

1. Setup: `dg_olc_script_copy()` deep-copies the original's `proto_script`
   into `OLC_SCRIPT(d)`, and the editor sets the draft's own `proto_script` to
   NULL. `OLC_ITEM_TYPE(d)` and the matching `OLC_MOB`/`OLC_OBJ`/`OLC_ROOM`
   must be set first, and `OLC_SCRIPT` must be empty.
2. Editing: `dg_script_menu()` lists the entries (flagging a trigger whose
   attach type does not match), and `dg_script_edit_parse()` inserts
   `position, vnum` (only existing vnums) or deletes an entry. While this menu
   is open, `OLC_MODE(d)` is the sentinel `OLC_SCRIPT_EDIT`.
3. Save: the editor's `*_save_internally()` frees the prototype's old list and
   installs `OLC_SCRIPT(d)` as the prototype's `proto_script`, then rebuilds
   live scripts as described in
   [What OLC does to live scripts](#what-olc-does-to-live-scripts).
4. Quit without saving: `OLC_SCRIPT` is attached to the draft (the room editor
   frees it with `free_proto_script()`), so it is freed with the draft and the
   prototype is untouched.

### Other immortal tools

* `tstat <vnum>` (`do_tstat()`): prototype listing, built with a bounded
  buffer that marks overflow.
* `stat` on a mobile, object or room prints its script via
  `do_sstat_character()`, `do_sstat_object()` or `do_sstat_room()`: globals,
  context, triggers, and for a waiting trigger its remaining wait, current
  line and locals.
* `tlist` lists triggers in a zone; `tcopy` copies a prototype through
  `trigedit_setup_existing()` and `trigedit_save()` (both in `oasis_*.c`, see
  [olc.md](olc.md)).
* `vdelete <var|all> <uid>`: delete globals from any entity.

### Invariants

* Only one descriptor edits a given trigger vnum at a time.
* A scratch trigger's `cmdlist` is borrowed and must never be freed or
  written through; `trigedit_save()` replaces it before use.
* After any change to `trig_index` order, every `nr` (prototypes, live
  triggers, other editors' drafts) must match the new positions before
  control returns to the game loop.
* `OLC_SCRIPT` belongs to the editor until saved, then to the prototype.

## Invariants checklist

Ownership and lifetime

* Prototypes own command lists; instances and drafts borrow them.
  `free_trigger()` never frees `cmdlist`.
* Locals belong to a run and are freed when the run ends, not when it waits.
* Globals belong to the script and die with it; players' globals are saved
  in the player file.
* Triggers and scripts extracted during a script run are retired and freed
  when the outermost `script_driver()` frame returns.
* Characters are freed at the end of the pulse; objects are freed at once.
* A `wait_event_data` is owned by its event and freed by `trig_wait_event()`
  or `event_cancel()`.

Identity

* UIDs: players `[0, 10M)`, mobiles `[10M, 20M)`, rooms `[20M, 30M)` (vnum
  plus base, never registered), objects `[30M, ...)`.
* Fetch IDs with `char_script_id()`/`obj_script_id()`; register and remove
  only through the lookup-table functions; removal is ownership-checked.
* Use `obj_uid_matches()` for possibly freed objects and (address, serial)
  for possibly freed triggers.

Dispatch and execution

* A trigger with non-zero `depth` never starts.
* `script_driver()` balances its depth counters on every return path and
  stops a frame whose own owner is freed or newly extracted.
* After any callback, re-acquire characters by UID, objects by UID and
  location, triggers by (address, serial) from live lists.
* Command buffers are `MAX_INPUT_LENGTH`; expansion is bounded.
* Player text is stored inert and restored only where it leaves the engine.

Events

* Do not cancel an event from inside its own callback.
* Clear every pointer to an event you cancel.
* Room wait payloads point into `world[]`; anything that moves `world[]` must
  repoint every room's waits.

OLC

* Saving a mobile or object prototype rebuilds every live instance's script
  (an object's old script is orphaned, not freed).
* Inserting a trigger prototype renumbers prototypes, live triggers and other
  drafts, but not `'T'` zone reset commands.
* `trigedit_save()` changes memory before it writes the disk.

## Pending changes on audit fix branches

The commits below exist only on the named branches and are **not** in this
branch. Each was read on its branch. Several other DG fixes listed by
`git log` on those branches are already present here in another form (for
example the per-frame owner check, the object dispatch snapshot, the bounded
`wait`, the room UID namespace check and the bounded `one_phrase()`); they are
not repeated.

| Branch | Commit | Behaviour change |
| --- | --- | --- |
| `worktree-wf_c613c2c5-f9f-34` (D2) | `1b47db3` | `event_process()` records the running event; `event_cancel()` on it frees only the payload, logs `event-cancel-running`, and leaves the struct to `event_process()`, which frees it without re-enqueueing. |
| `worktree-wf_c613c2c5-f9f-34` (D2) | `cdcd2c3` | `event_free_all()` sets `event_q = NULL`; later `event_create()` aborts with a fatal diagnostic, `event_cancel()` and `event_process()` report `event-queue-uninit` and do nothing. |
| `worktree-wf_c613c2c5-f9f-34` (D2) | `314faed` | `event_create()` warns (`event-when-clamped`) when it clamps a delay below 1. |
| `worktree-wf_c613c2c5-f9f-22` | `6e63025` | Room waits record the room vnum; `trig_wait_event()` resolves it with `real_room()` and checks the trigger is still attached there, instead of comparing a pointer into `world[]`. (This branch is not the one the merge plan names for D2.) |
| `worktree-wf_c613c2c5-f9f-33` (D1) | `4e94226` | `extract` stops when the words run out instead of looping `num` times (a large `num` stalled the game loop), and reports `extract-range`. |
| `worktree-wf_c613c2c5-f9f-33` (D1) | `ddf55a2` | `is_killer`/`is_thief` setters are ignored (with `field-target`) on mobiles; on a mob they set the `MOB_` flag with the same bit, because `PLR_FLAGS` and `MOB_FLAGS` share one array. |
| `worktree-wf_c613c2c5-f9f-33` (D1) | `ffb7950` | Field setters and `script_damage()` compute in `long long` and clamp to the field's type (reporting `field-clamp`), instead of wrapping `sh_int` fields such as hit points. |
| `worktree-wf_c613c2c5-f9f-33` (D1) | `f1908cf` | Saturated arithmetic and division by zero in expressions report `script-arith` (values unchanged). |
| `worktree-wf_c613c2c5-f9f-35` (E1) | `ff52f8f` | `oforce all` and `wforce all` walk a UID snapshot of the room and re-acquire each occupant, instead of following `next_in_room` across forced commands. |
| `worktree-wf_c613c2c5-f9f-35` (E1) | `534163f` | One door editor (`dg_door_edit()`) for `mdoor`/`odoor`/`wdoor`; a newly created exit leads `NOWHERE` with no key (here it leads to rnum 0), and key and room values are parsed strictly. |
| `worktree-wf_c613c2c5-f9f-35` (E1) | `6571986` | `osetval` refuses a liquid type or weapon attack type outside its table (`osetval-range`). |
| `worktree-wf_c613c2c5-f9f-32` (C2) | `cfa23ca` | A `T` line naming a missing trigger keeps the vnum in `proto_script` (only instantiation is skipped), so an OLC save no longer deletes it from disk. The same change is `59940a4` on `worktree-wf_c613c2c5-f9f-35`. With this change `dg_script_menu()` would meet unresolvable vnums; `worktree-wf_c613c2c5-f9f-28` replaces its unchecked `trig_index` lookup with a checked one that prints "Missing trigger". |
| `worktree-wf_c613c2c5-f9f-32` (C2) | `eed9ae3` | `parse_trigger()` loads an empty command body as `* No script` with a `trigger-parse-empty` warning, instead of crashing on `strdup(NULL)`. |

## Further reading

* [../maintainer-safety-guide.md](../maintainer-safety-guide.md): error
  reporting (`TB_ERROR_*`), breadcrumbs, and the callback rules in full.
* [../macro-audit.md](../macro-audit.md): the history of the DG callback
  lifetime audit, passes 1 to 13.
* [overview.md](overview.md): the game loop and the pulse schedule that calls
  `event_process()`, `script_trigger_check()` and `check_time_triggers()`.
* [gameplay.md](gameplay.md): extraction
  ([Character and object lifetime](gameplay.md#character-and-object-lifetime)),
  mud events ([Events and generic lists](gameplay.md#events-and-generic-lists)),
  [Combat](gameplay.md#combat) and
  [Special procedures](gameplay.md#special-procedures), the C alternative to
  triggers.
* [olc.md](olc.md): the OLC framework around `trigedit` and the
  attached-trigger editor.
* [persistence.md](persistence.md): boot order and the world file loaders
  that call `parse_trigger()`, `dg_read_trigger()` and `dg_obj_trigger()`.
* [foundations.md](foundations.md): `structs.h`, the `utils.h` macros, and the
  structured error reporters.
* [../../doc/dg_events.txt](../../doc/dg_events.txt): the original
  descriptions of the Death Gate event system and the mud event layer.
* In game: `help trigedit`, `help dg-scripts` and the variable and command
  references they link to.
