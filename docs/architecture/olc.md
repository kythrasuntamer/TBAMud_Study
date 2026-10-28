# OasisOLC: online creation

OasisOLC is the set of in-game editors that let builders change the world
while the MUD is running: rooms, objects, mobiles, zones and their reset
commands, shops, quests, triggers, socials, help entries, text files, combat
messages, player preferences and the game configuration. This document
explains how an editing session is attached to a connection, the lifecycle
every editor follows, what each editor does, how the generic "GenOLC"
library changes the in-memory tables and writes the world files, how real
numbers are kept consistent when those tables grow and shrink, and who is
allowed to edit what.

It describes the code at commit `aeeb58d` of
`audit/macro-inventory-and-dg-followup-20261027`. Many fixes from the
October 2026 audit exist only on the audit fix branches; they are listed in
[Pending changes on audit fix branches](#pending-changes-on-audit-fix-branches)
so that branch behaviour is never mistaken for what this code does today.
Trigger editing is covered in [dg-scripts.md](dg-scripts.md#trigger-olc);
the shop and quest runtime is covered in [gameplay.md](gameplay.md).

Two words are used throughout:

- A **vnum** (virtual number) is the stable number a builder types and the
  world files store. Vnums never change.
- An **rnum** (real number) is an index into an in-memory array such as
  `world[]`, `obj_proto[]` or `zone_table[]`. Those arrays are kept sorted
  by vnum, so inserting or deleting an entry shifts every rnum above it.
  Much of OLC's complexity comes from keeping stored rnums correct.

`IDXTYPE` is `unsigned short` (`CIRCLE_UNSIGNED_INDEX` is 1 in `structs.h`),
so `NOWHERE`, `NOTHING` and `NOBODY` are all 65535, the largest value the
type can hold. Any code that shifts rnums with `x += (x >= n)` or
`x -= (x > n)` must exclude the sentinel explicitly, because the sentinel
compares greater than every real rnum.

## Source files

| File | Role |
| --- | --- |
| `oasis.h` | `struct oasis_olc_data`, the `OLC_*` accessor macros, cleanup types, the sub-mode constants of each editor, permission sentinels and the editors' public prototypes. |
| `oasis.c` | `cleanup_olc()`, `can_edit_zone()`, `send_cannot_edit()`, `split_argument()` and the OLC colour pointers. |
| `oasis_copy.c` | `rcopy`, `ocopy`, `mcopy`, `scopy`, `tcopy` (`do_oasis_copy()`), `dig` and `buildwalk`. |
| `oasis_delete.c` | `free_strings()` for room and configuration copies. |
| `oasis_list.c` | `rlist`, `olist`, `mlist`, `tlist`, `slist`, `qlist`, `zlist` and their helpers. |
| `redit.c` | Room editor. |
| `oedit.c` | Object prototype editor, plus `iedit`, the live-instance editor. |
| `medit.c` | Mobile prototype editor. |
| `zedit.c` | Zone header and reset-command editor; zone creation. |
| `sedit.c` | Shop editor. |
| `qedit.c` | Quest editor. |
| `cedit.c` | Game configuration editor. |
| `aedit.c` | Social (action) editor and `astat`. |
| `hedit.c`, `hedit.h` | Help editor, `helpcheck` and `hindex`. |
| `tedit.c` | Text file editor (news, MOTD, credits and others). |
| `prefedit.c`, `prefedit.h` | Player preference and toggle editor. |
| `msgedit.c`, `msgedit.h` | Combat message loader, saver and editor. |
| `dg_olc.c`, `dg_olc.h` | `trigedit` and the trigger-list editor shared by `medit`, `oedit` and `redit` (see [dg-scripts.md](dg-scripts.md#trigger-olc)). |
| `genolc.c`, `genolc.h` | The save list, `save_all()`, string and extra-description helpers, zone export. |
| `genwld.c`, `genwld.h` | Rooms: add, delete, save, copy. |
| `genobj.c`, `genobj.h` | Object prototypes: add, insert, renumber, delete, save; the `oset` helpers. |
| `genmob.c`, `genmob.h` | Mobile prototypes: add, delete, save, copy, free. |
| `genzon.c`, `genzon.h` | Zones: create, index files, save, reset-command list helpers. |
| `genshp.c`, `genshp.h` | Shops: copy, add, save. |
| `genqst.c` | Quests: add, delete, save. |
| `modify.c` | `string_write()` and `string_add()`, the multi-line string editor OLC hands long text to. |
| `interpreter.c` | `nanny()`, which routes input for the OLC connection states to each editor's parse function. |
| `comm.c` | `game_loop()` input routing and `close_socket()`, which cleans up OLC on link loss. |

## How OLC fits into the server

An OLC session belongs to a **descriptor** (a network connection), not to a
character. A builder starts it by typing an editor command while playing.
The command allocates `d->olc`, builds a private working copy of the thing
being edited (the **scratch copy**) and changes the descriptor's connection
state to the editor's `CON_*EDIT` state. From then on every line the builder
types goes to the editor instead of the command interpreter:

```
one input line for descriptor d          (comm.c:game_loop())
  |
  +-- d->showstr_count set? ----------> show_string()        pager
  +-- d->str set? --------------------> string_add()         multi-line string editor
  +-- STATE(d) != CON_PLAYING? -------> nanny()
  |      |
  |      +-- state listed in olc_functions[] --> <editor>_parse(d, line)
  |      +-- otherwise: login, menu and password states
  +-- otherwise ----------------------> command_interpreter()
```

`interpreter.c:nanny()` holds the dispatch table `olc_functions[]`, which
maps `CON_OEDIT`, `CON_IEDIT`, `CON_ZEDIT`, `CON_SEDIT`, `CON_MEDIT`,
`CON_REDIT`, `CON_CEDIT`, `CON_TRIGEDIT`, `CON_AEDIT`, `CON_HEDIT`,
`CON_QEDIT`, `CON_PREFEDIT`, `CON_IBTEDIT` and `CON_MSGEDIT` to their parse
functions. `CON_TEDIT` is not in the table: `tedit` lives entirely inside
the string editor, so its input never reaches `nanny()`.

While a builder edits, the character stays in the world. `IS_PLAYING()`
(`utils.h`) treats every state from `FIRST_OLC_STATE` (`CON_OEDIT`) to
`LAST_OLC_STATE` (`CON_IEDIT`) as playing, the room sees "$n starts using
OLC." and `PLR_WRITING` is set. The builder cannot move or act because their
input goes to the editor, but the world keeps running around them, and other
code can still act on the character: a higher immortal's `force` calls
`command_interpreter()` directly, whatever the descriptor's state.

### Invariants

- OLC input is processed one line at a time, from the game loop, between
  pulses. No editor code runs inside a DG script callback or a timer, so a
  scratch copy cannot change underneath an editor during one parse call.
- Live data can change between two lines. Anything an editor keeps across
  lines that points into live tables (an rnum, a live object pointer) can go
  stale and has to be either renumbered by whoever changes the table or
  revalidated before use.

## Descriptor OLC state

`struct oasis_olc_data` (`oasis.h`) is the per-session workspace. It is
allocated zeroed with `CREATE()` by the editor's entry command and freed by
`oasis.c:cleanup_olc()`. Each editor uses only a few of its fields.

| Field | Accessor | Type | Meaning |
| --- | --- | --- | --- |
| `mode` | `OLC_MODE(d)` | `int` | The prompt the next input line answers: one of the editor's sub-mode constants (`REDIT_NAME`, `ZEDIT_ARG1`, ...), or `OLC_SCRIPT_EDIT` while the trigger-list editor is open. |
| `zone_num` | `OLC_ZNUM(d)` | `zone_rnum` | The zone being edited, as an **rnum**. `NOWHERE` for `iedit`. In `aedit` and `hedit` it is an index into `soc_mess_list[]` or `help_table[]`, not a zone. |
| `number` | `OLC_NUM(d)` | `room_vnum` | The **vnum** of the thing being edited. Despite the type it holds object, mobile, shop, quest and trigger vnums too. In `msgedit` it is a slot index into `fight_messages[]`. |
| `value` | `OLC_VAL(d)` | `int` | Usually the "something was modified" flag that makes Quit ask "Do you wish to save?". Some editors reuse it: `redit` stores the exit direction in it while an exit menu is open. |
| `storage` | `OLC_STORAGE(d)` | `char *` | The script text being edited in `trigedit`; the word typed in `aedit` and `hedit`; the file name in `tedit`. |
| `room` | `OLC_ROOM(d)` | `struct room_data *` | Scratch room (`redit`; also `dig` and `buildwalk`). |
| `obj` | `OLC_OBJ(d)` | `struct obj_data *` | Scratch object (`oedit`, `iedit`). |
| `iedit_obj`, `iedit_uid` | `OLC_IEDIT_OBJ(d)`, `OLC_IEDIT_UID(d)` | `struct obj_data *`, `long` | The live object `iedit` will write back to, and its UID. The pointer is borrowed and may dangle; it is only ever compared through `obj_uid_matches()`. |
| `mob` | `OLC_MOB(d)` | `struct char_data *` | Scratch mobile (`medit`). |
| `zone` | `OLC_ZONE(d)` | `struct zone_data *` | Scratch zone header and command list (`zedit`). |
| `shop` | `OLC_SHOP(d)` | `struct shop_data *` | Scratch shop (`sedit`). |
| `config` | `OLC_CONFIG(d)` | `struct config_data *` | Scratch copy of `config_info` (`cedit`). |
| `quest` | `OLC_QUEST(d)` | `struct aq_data *` | Scratch quest (`qedit`). |
| `desc` | `OLC_DESC(d)` | `struct extra_descr_data *` | A **cursor** into the scratch room's or object's extra-description list, not a separate allocation. |
| `action` | `OLC_ACTION(d)` | `struct social_messg *` | Scratch social (`aedit`). |
| `trig` | `OLC_TRIG(d)` | `struct trig_data *` | Scratch trigger (`trigedit`). |
| `prefs` | `OLC_PREFS(d)` | `struct prefs_data *` | Scratch preferences (`prefedit`). |
| `ibt` | `OLC_IBT(d)` | `struct ibt_data *` | Scratch idea/bug/typo record (`ibtedit`, `ibt.c`). |
| `msg`, `m_type` | `OLC_MSG_LIST(d)`, `OLC_MSG(d)` | `struct message_list *`, `struct message_type *` | Scratch combat message set (`msgedit`) and a cursor into it. |
| `script_mode`, `trigger_position`, `item_type` | `OLC_SCRIPT_EDIT_MODE(d)`, -, `OLC_ITEM_TYPE(d)` | `int` | State of the trigger-list editor (`dg_olc.h`): its prompt, and whether the list belongs to a mob, object or room. |
| `script` | `OLC_SCRIPT(d)` | `struct trig_proto_list *` | The session's private copy of the edited entity's trigger vnum list (`medit`, `oedit`, `redit`). |
| `help` | `OLC_HELP(d)` | `struct help_index_element *` | Scratch help entry (`hedit`). |

`OLC_EXIT(d)` is `OLC_ROOM(d)->dir_option[OLC_VAL(d)]`, the exit `redit`'s
exit menu is working on.

### Invariants

- The `OLC_*` macros dereference `d->olc` without a NULL check. Use them only
  while the descriptor is in an OLC state, or inside an entry command after
  it has allocated `d->olc`.
- In the world editors `OLC_NUM(d)` is a vnum and never needs renumbering,
  and `OLC_ZNUM(d)` is a zone rnum (in `aedit`, `hedit` and `msgedit` they
  are table indices instead; see those editors); zone rnums only change when `zedit new` inserts a zone, and that path
  renumbers other sessions (see [zedit](#zedit-zones-and-reset-commands)).
- Scratch copies are private to their session. The only code outside an
  editor that looks into another descriptor's scratch copy is the
  renumbering in other editors' save paths, which uses `STATE(dsc)` to decide
  which scratch field is valid. A scratch copy reachable from a descriptor
  that is no longer in the matching state is never renumbered.
- `OLC_DESC(d)` and `OLC_MSG(d)` are cursors. Freeing one without unlinking
  it from the list it points into corrupts that list.
- A descriptor normally has `d->olc` set exactly while it is in an OLC state.
  `rcopy`, `dig` and `buildwalk` briefly allocate `d->olc` while the
  descriptor stays `CON_PLAYING`; they clean it up before returning.

## The editor lifecycle

Every editor follows the same six stages. `redit` is the reference example;
the other world editors (`oedit`, `medit`, `sedit`, `zedit`, `qedit`,
`trigedit`) differ only in detail.

```
do_oasis_redit(ch, "1234")                            entry
  |-- return silently if NPC, no descriptor, or not CON_PLAYING
  |-- parse the arguments ("save [zone]" sets a save flag)
  |-- refuse if another CON_REDIT descriptor has OLC_NUM == 1234
  |-- CREATE(d->olc); OLC_ZNUM = real_zone_by_thing(1234)
  |-- can_edit_zone(ch, OLC_ZNUM) or refuse (send_cannot_edit)
  |-- save flag set? --> save_rooms(OLC_ZNUM); free(d->olc); return
  |-- OLC_NUM = 1234
  |-- redit_setup_existing(d, rnum) or redit_setup_new(d)      setup
  |-- STATE(d) = CON_REDIT; PLR_WRITING; "$n starts using OLC."
  v
redit_parse(d, line), once per input line                   parse
  |-- switch (OLC_MODE(d)) ... display functions set the next mode
  |-- Q, nothing changed --------------------------> cleanup_olc(d, CLEANUP_ALL)
  |-- Q, OLC_VAL set --> "Do you wish to save?"
        |-- N --------------------------------------> cleanup_olc(d, CLEANUP_ALL)
        |-- Y --> redit_save_internally(d)                  save in memory
                  if CONFIG_OLC_SAVE: save_rooms(zone)      save to disk
                  cleanup_olc(d, CLEANUP_ALL)               cleanup
```

### 1. Entry

The entry commands (`do_oasis_redit()`, `do_oasis_oedit()` and so on) run
these checks, in this order:

1. **Return silently** if the character is an NPC, has no descriptor, or
   the descriptor is not `CON_PLAYING`. This stops an immortal switched
   into a mobile from building, and stops a forced command
   (`act.wizard.c:do_force()` runs `command_interpreter()` on its victim
   whatever the victim's state) from opening a second editor on a
   descriptor that is already in one. A builder who is `CON_PLAYING` can
   still be forced into an editor. `qedit`, `tedit`, `prefedit` and
   `msgedit` skip this check; see their sections.
2. **The save form.** `<editor> save [zone]` is recognised while the
   arguments are parsed. After the zone and permission checks below it
   writes that zone's file, frees the workspace and returns without opening
   a session. With no zone argument the builder's OLC zone is used, but see
   the note below.
3. **Range check** on the typed vnum (`IDXTYPE_MIN` to `IDXTYPE_MAX`).
4. **Same-target guard.** If another descriptor is in the same state with
   the same `OLC_NUM`, the command refuses. Nothing stops two different
   editors from touching related data (a `zedit` session and a `redit`
   session on rooms of the same zone, for example); the renumbering loops
   exist for that reason.
5. **Stale workspace.** If `d->olc` is already set, the command logs
   "Player already had olc structure" and calls plain `free(d->olc)`, which
   leaks any scratch data still attached to it.
6. **Zone lookup** with `genzon.c:real_zone_by_thing()` (the zone whose
   room range contains the vnum) and **permission** with
   `oasis.c:can_edit_zone()`; see [Builder permissions](#builder-permissions).

Note on the save form: `redit`, `oedit`, `medit`, `sedit` and `zedit` turn
a missing zone argument into the bottom **room vnum** of the builder's OLC
zone and then look that number up with `real_zone()`, which expects a
**zone vnum**. For a zone such as 30 (rooms 3000-3099) this finds no zone
and the command answers "Sorry, there is no zone for that number!"; for a
low zone it can name an unrelated zone. `qedit` does the opposite: it always
uses `real_zone_by_thing()`, so its no-argument form works but
`qedit save 30` saves the zone containing *room* 30. Passing an explicit
zone number to the first five editors works as intended.

### 2. Setup

`<editor>_setup_new()` builds a scratch copy with default values.
`<editor>_setup_existing()` copies the live entity. The world editors follow
the same copy rules:

- Strings are duplicated (`str_udup()`, `strdup()`), so the scratch copy
  owns its own text. Exits and extra descriptions are deep-copied.
- Lists that tie the entity into the live world are cleared in the copy:
  `redit` clears `people`, `contents` and `events`.
- The running script is never copied: `SCRIPT(scratch)` is set to NULL.
- The trigger vnum list is copied into `OLC_SCRIPT(d)` by
  `dg_olc.c:dg_olc_script_copy()`, and the scratch copy's own
  `proto_script` is set to NULL.
- Everything else (numbers, flags, function pointers, counters) arrives by
  plain struct assignment and is therefore a snapshot of the moment the
  editor opened.

### 3. Parse

`<editor>_parse()` is a state machine on `OLC_MODE(d)`. Display functions
print a menu and set the mode the next line will answer. Most numeric
prompts read the line with `tb_parse_int_or_zero()`, so a non-numeric answer
reads as 0, and clamp with `LIMIT()` (`genolc.h`) without telling the
builder. Long text goes to the string editor (see
[The string editor hand-off](#the-string-editor-hand-off)). The `S` choice
of `medit`, `oedit` and `redit` opens the trigger-list editor
(`dg_olc.c:dg_script_menu()`), which sets `OLC_MODE(d)` to
`OLC_SCRIPT_EDIT`; the parse function forwards input to
`dg_olc.c:dg_script_edit_parse()` until it returns 0.

`redit`, `oedit`, `medit`, `sedit` and `qedit` end their parse functions with
`OLC_VAL(d) = 1` and a redisplay of the main menu, so in those editors any
answered prompt, even one that changed nothing, marks the session modified.

### 4. Save in memory

`<editor>_save_internally()` installs the scratch copy into the live tables
through the GenOLC library (`add_room()`, `add_object()`, `add_mobile()`,
`add_shop()`, `add_quest()`), then re-attaches triggers, then, only when the
save inserted a new entry, renumbers the scratch copies of other open
sessions. The scratch copy keeps its own strings: GenOLC copies them again.

### 5. Save to disk

If `CONFIG_OLC_SAVE` (`config_info.operation.auto_save_olc`, default `YES`,
set with `cedit`) is on, the editor calls the zone writer immediately
(`save_rooms()`, `save_objects()`, ...). Either way, the GenOLC `add_*`
function has already put the zone and file type on the **save list**
(`genolc.c:add_to_save_list()`). The `olc` command
(`genolc.c:do_show_save_list()`) lists pending saves; `<editor> save <zone>`,
`saveall` (`act.wizard.c:do_saveall()`) and shutdown (`comm.c:main()` calls
`save_all()` after the game loop unless `circle_reboot == 2`) write them.
Each writer removes its own entry from the save list when it succeeds.

### 6. Cleanup

`oasis.c:cleanup_olc(d, type)` releases whatever is attached to `d->olc`,
removes `PLR_WRITING`, tells the room "$n stops using OLC.", logs the end of
the session, sets `STATE(d)` back to `CON_PLAYING` and frees `d->olc`. It
returns at once if `d->olc` is NULL.

| Workspace field | `CLEANUP_ALL` | `CLEANUP_STRUCTS` | `CLEANUP_CONFIG` |
| --- | --- | --- | --- |
| `OLC_ROOM` | `free_proto_script()` and `redit.c:free_room()` (strings, script, trigger list, struct) | `free()` of the struct only | see below |
| `OLC_OBJ` | `free_object_strings()` and `free()` | same | same |
| `OLC_MOB` | `genmob.c:free_mobile()` | same | same |
| `OLC_ZONE` | builders, name, command array and struct | same | same |
| `OLC_SHOP` | `genshp.c:free_shop()` | same | same |
| `OLC_QUEST` | `free_quest()` | `free()` of the struct only | nothing |
| `OLC_ACTION` | `free_action()` | `free()` of the struct only | nothing |
| `OLC_HELP` | `free_help()` | `free()` of the struct only | nothing |
| `OLC_IBT` | `free_olc_ibt()` | same | same |
| `OLC_MSG_LIST` | `free_message_list()` | same | same |
| `OLC_STORAGE` | `free()` | same | same |
| `OLC_TRIG` | `free_trigger()` (the command list is shared with the prototype and not freed) | same | same |
| `OLC_PREFS` | `free()` | same | same |
| `OLC_SCRIPT` | never freed here | never | never |
| `OLC_CONFIG` | not freed | not freed | `free_config()`, but only inside the `if (OLC_ROOM(d))` block |

`CLEANUP_STRUCTS` exists for editors that have already moved the scratch
copy's strings into the live data, so only the containers must go. In
`cleanup_olc()` the `CLEANUP_CONFIG` case is written inside the
`switch (cleanup_type)` that runs only `if (OLC_ROOM(d))`. A `cedit` session
has no scratch room, so `cleanup_olc()` itself never frees `OLC_CONFIG`;
see [cedit](#cedit-game-configuration) for what `cedit` does about it.

`OLC_SCRIPT` is never freed by `cleanup_olc()`. The comment there says it
"is always set as trig_proto of OLC_OBJ/MOB/ROOM", which holds only on the
paths that hand it over: a save gives it to the prototype, and the "don't
save" answer of `redit`, `oedit` and `medit` attaches it to the scratch copy
so that `cleanup_olc()` frees it with the copy. Quitting with no changes,
losing the link and the `W` copy choice leak it.

### Other ways a session ends

- **Link loss.** `comm.c:close_socket()` calls
  `cleanup_olc(d, CLEANUP_ALL)` for `CON_OEDIT`, `CON_REDIT`, `CON_ZEDIT`,
  `CON_MEDIT`, `CON_SEDIT`, `CON_TEDIT`, `CON_TRIGEDIT`, `CON_AEDIT`,
  `CON_HEDIT`, `CON_QEDIT` and `CON_MSGEDIT`. It does **not** for
  `CON_CEDIT`, `CON_PREFEDIT`, `CON_IBTEDIT` or `CON_IEDIT`: those
  workspaces leak, and `PLR_WRITING` stays set on the linkless character. If
  the builder was in the string editor, `d->backstr` (the copy kept for
  `/a`bort) is not freed either, because `close_socket()` frees it only when
  `PLR_WRITING` is clear.
- **The character is extracted.** `handler.c:extract_char_final()` sends a
  descriptor whose player is extracted to `CON_MENU` without calling
  `cleanup_olc()`. The workspace leaks and the next editor command logs
  "already had olc structure".
- **A forced command.** Most entry commands refuse a descriptor that is not
  `CON_PLAYING`. `do_dig()` does not: forcing a builder who is inside an
  editor to `dig` to a missing room frees the open workspace with a plain
  `free()` and ends the session. `qedit`, `tedit`, `prefedit` and `msgedit`
  do not check either, with the consequences described in their sections.

### Invariants

- A session's scratch data is freed exactly once, by `cleanup_olc()`. The
  exceptions above (`OLC_SCRIPT`, `OLC_CONFIG`, the link-loss states, player
  extraction) leak rather than double-free.
- After `<editor>_save_internally()` the live tables own copies of the
  scratch strings, and the scratch copy still owns its own. The trigger list
  `OLC_SCRIPT` is the one thing that changes owner on save.
- The entry checks are the only place permission is checked. Nothing
  re-checks `can_edit_zone()` at save or delete time.

## The string editor hand-off

Room, object, mobile and extra descriptions, exit descriptions, help text,
quest text, trigger scripts and text files are typed in the multi-line
string editor of `modify.c`. The editor is generic; OLC plugs into it in two
places.

1. **Start.** The editor calls
   `modify.c:string_write(d, &field, max_len, 0, oldtext)`. `d->str` is set
   to the **address of the scratch field itself**, `d->max_str` to the size
   limit, and `oldtext` (a `strdup()` of the current text, or NULL) becomes
   `d->backstr` when the improved editor is compiled in. `PLR_WRITING` is set.
   From now on `comm.c:game_loop()` sends every line to `string_add()`
   instead of the editor's parse function; `OLC_MODE(d)` stays at the
   editor's "editing description" mode.
2. **Lines.** `modify.c:string_add()` appends to `*d->str`, allocating it on
   the first line and growing it with `RECREATE()`. Text longer than
   `d->max_str` is truncated or the line is skipped.
3. **Finish.** On save (`@` or `/s`) `d->backstr` is freed and an empty
   result becomes "Nothing.\r\n". On abort (`/a`), for the OLC states and
   `CON_PLR_DESC`, the new text is freed and `*d->str = d->backstr` restores
   the old one. Then `string_add()` calls the editor's cleanup callback from
   its `cleanup_modes[]` table, which normally redisplays a menu based on
   `OLC_MODE(d)`, and finally clears `d->str` and `PLR_WRITING`.

| State | Cleanup callback |
| --- | --- |
| `CON_CEDIT` | `cedit_string_cleanup()` |
| `CON_MEDIT` | `medit_string_cleanup()` |
| `CON_OEDIT`, `CON_IEDIT` | `oedit_string_cleanup()` |
| `CON_REDIT` | `redit_string_cleanup()` |
| `CON_TEDIT` | `tedit_string_cleanup()` |
| `CON_TRIGEDIT` | `trigedit_string_cleanup()` |
| `CON_HEDIT` | `hedit_string_cleanup()` |
| `CON_QEDIT` | `qedit_string_cleanup()` |
| `CON_IBTEDIT` | `ibtedit_string_cleanup()` |

`CON_ZEDIT`, `CON_SEDIT`, `CON_AEDIT`, `CON_PREFEDIT` and `CON_MSGEDIT` have
no callback; those editors take all their text on a single prompt line.

### Invariants

- The string being edited is owned by the scratch structure, not by the
  string editor. `d->str` is a `char **` into that structure, so the
  structure must stay allocated, and must not be reallocated, until the
  string editor finishes. Nothing in an editor may free or replace the
  scratch copy while `d->str` is set.
- `*d->str` may be NULL until the first line is typed; menu code that
  prints a description must allow for NULL.
- The cleanup callback must not free `*d->str`; `string_add()` already
  decided whether the new or the old text survives.
- `string_add()` clears `PLR_WRITING` when the text is finished even though
  the OLC session continues, so after the first long-text prompt the "who"
  list no longer shows the builder as writing.

## Editors

### redit (rooms)

| | |
| --- | --- |
| Command | `redit [vnum]`, `redit save [zone]`; `LVL_BUILDER`. No argument edits the current room. |
| State and scratch | `CON_REDIT`; `OLC_ROOM`, `OLC_SCRIPT`, `OLC_DESC` (cursor). |
| Live data changed | `world[]` through `genwld.c:add_room()` / `delete_room()`. |
| Writer | `genwld.c:save_rooms()` (`redit_save_to_disk()` is a one-line wrapper that is passed a zone **rnum** despite its `zone_vnum` parameter). |

**Setup.** `redit_setup_existing()` struct-copies `world[rnum]`, clears
`people`, `contents` and `events`, duplicates name and description, deep
copies every exit and extra description, copies the trigger list into
`OLC_SCRIPT` and clears `proto_script` and `SCRIPT`. The extra-description
copy calls `strdup()` on each keyword and description without a NULL check,
so a room loaded from a file with an empty extra-description body crashes
the editor. `redit_setup_new()` gives the room a default name and
description and sets `number` to `NOWHERE`, which is how the save path
recognises a new room.

**Menus.** Name (rejected when longer than `MAX_ROOM_NAME / 2` visible
characters), description (string editor, `MAX_ROOM_DESC`), flags, sector,
one exit menu per direction (the diagonal ones only with
`CONFIG_DIAGONAL_DIRS`), extra descriptions, `S` trigger list, `W` copy,
`X` delete, `Q` quit. In an exit menu the target room is typed as a vnum and
stored as an **rnum** (`real_room()`; `-1` stores `NOWHERE`); the key is a
vnum (`NOTHING` if negative); door flags are chosen from five combinations
of `EX_ISDOOR`, `EX_PICKPROOF` and `EX_HIDDEN`. Leaving the extra
description menu with `0` removes an entry whose keyword or description is
still NULL. Opening any exit menu stores the direction in `OLC_VAL(d)`,
which also marks the room modified.

**Save.** `redit_save_internally()` sets the scratch room's `number` to
`OLC_NUM(d)` and its `zone` to `OLC_ZNUM(d)` and calls `add_room()`. It then
frees the room's old trigger list if it is still there and differs from
`OLC_SCRIPT`, installs `OLC_SCRIPT` as the room's `proto_script` and calls
`assign_triggers()`. If the scratch `number` was `NOWHERE` before the save
it renumbers other sessions: for every other descriptor in `CON_ZEDIT` it
shifts room rnums at or above the new room in `'M'`, `'O'`, `'T'`, `'V'`
(argument 3) and `'D'`, `'R'` (argument 1) commands, and for every other
descriptor in `CON_REDIT` it shifts exit targets, never moving `NOWHERE`
(commit `f908742`).

What a save does to the live room is decided by `add_room()` and
`copy_room()`:

- The room's running script is extracted (its variables and any pending
  `wait` are lost) and a new one is built from the new trigger list.
- People and contents are kept: `add_room()` saves and restores the two
  list heads around `copy_room()`.
- The room's mud-event list is **dropped**: `copy_room()` sets
  `to->events = from->events`, and the scratch copy's list is always NULL.
  An active darkness spell keeps running from the event queue but can no
  longer be found or cancelled through the room.
- The light counter `light`, the special procedure `func` and every other
  plain field come from the scratch copy, which captured them when the
  editor opened. If a lit character entered or left the room while it was
  being edited, the room's light count is now wrong for good.
- The old trigger list is **leaked**: `copy_room()`'s struct assignment
  overwrites `proto_script` with the scratch copy's NULL before
  `redit_save_internally()` looks for it, so its "free the old list" test is
  always false on an update.

**Copy (`W`).** `redit_setup_existing()` is called for the source room on
top of the current scratch copy, leaking it and its trigger list copy. The
scratch room's `number` is now the source vnum, not `NOWHERE`, so when the
copy is saved into a vnum that does not exist yet, `add_room()` inserts a
room but `redit_save_internally()` treats the save as an update and skips
the renumbering of other sessions. `rcopy` has the same problem.

**Delete (`X`).** `delete_room(real_room(OLC_NUM(d)))` removes the room
being edited (commit `15fabbe` made it use `OLC_NUM` rather than the scratch
vnum, which after a copy names the source). See
[genwld.c](#genwldc-rooms) for what deletion does to the world. Other
sessions are not renumbered after a delete.

### oedit (object prototypes)

| | |
| --- | --- |
| Command | `oedit <vnum>`, `oedit save [zone]`; `LVL_BUILDER`. |
| State and scratch | `CON_OEDIT`; `OLC_OBJ`, `OLC_SCRIPT`, `OLC_DESC` (cursor). |
| Live data changed | `obj_proto[]`, `obj_index[]` and every live instance, through `genobj.c:add_object()` / `delete_object()`. |
| Writer | `genobj.c:save_objects()`. |

**Setup.** `oedit_setup_existing()` allocates an object and fills it with
`genobj.c:copy_object()` (struct copy plus duplicated strings and extra
descriptions), copies the trigger list into `OLC_SCRIPT` and clears
`SCRIPT` and `proto_script`. `oedit_setup_new()` uses `clear_object()`,
default strings and `ITEM_WEAR_TAKE`.

**Menus.** Keywords, short, long and action descriptions, type, extra flags,
wear flags, weight, cost, rent, timer, the four values (prompts depend on
the type), applies, extra descriptions, minimum level, permanent affects,
`S` trigger list, `W` copy, `X` delete, `Q` quit.

**Save.** `oedit_save_internally()` remembers whether the vnum is new, calls
`add_object()`, frees the prototype's old trigger list if it is still there
(it is not, for the same reason as in `redit`, so the old list leaks) and
installs `OLC_SCRIPT` as the prototype's `proto_script`. It then walks
`object_list`: for each instance of the prototype it extracts the running
script if there is one, frees the instance's trigger list, gives it a fresh
copy of the prototype's (`copy_proto_script()`) and calls
`assign_triggers()`. For a new prototype it finally renumbers other
sessions: product rnums of `CON_SEDIT` sessions at or above the new rnum,
and object rnums in `'P'` (arguments 1 and 3), `'E'`, `'G'`, `'O'`
(argument 1) and `'R'` (argument 2) commands of `CON_ZEDIT` sessions.

The instance loop runs after `add_object()` has already rewritten every
instance (see
[What a save does to live instances](#what-a-save-does-to-live-instances)).
That rewrite replaced each instance's `script` pointer with the prototype's
NULL, so the `extract_script()` in the loop never runs: the old scripts are
orphaned, not freed. Their triggers stay allocated and on the global
trigger list, and a trigger that was in a `wait` resumes on the object when
the wait expires, because `dg_scripts.c:trig_wait_event()` only checks that
the object is still in `object_list`.

**Copy and delete** behave as in `redit`: `W` leaks the replaced scratch
copy and trigger list; `X` calls `delete_object(real_object(OLC_NUM(d)))`.

### iedit (one live object)

`iedit` edits a single object instance in the game instead of a prototype.
It reuses `oedit`'s menus and parse function.

| | |
| --- | --- |
| Command | `iedit <object>`; `LVL_BUILDER`. The object is found in the builder's equipment, inventory or room with `generic_find()`. |
| State and scratch | `CON_IEDIT`; `OLC_OBJ` (scratch), `OLC_IEDIT_OBJ` (borrowed live pointer), `OLC_IEDIT_UID`. `OLC_ZNUM` is `NOWHERE`. |
| Live data changed | The one object, in place. |
| Writer | None. The change persists only through the owner's rent or crash file (`PLR_CRASH` is set on a player owner) or a house file; an edited object lying in an ordinary room is lost at reboot. |

`do_oasis_iedit()` does **not** call `can_edit_zone()`: any builder can
edit any object they can reach. It records the object's pointer and its UID
(`obj_script_id()`), builds the scratch copy with `copy_object()` and clears
its `SCRIPT`, `proto_script` and `OLC_SCRIPT`. `iedit_parse()` refuses the
`S`, `W` and `X` main-menu choices and passes everything else to
`oedit_parse()`, so type, flags, weight, values, applies and permanent
affects can all be changed (`IEDIT_NOTES.md` says values are blocked; the
code no longer does that).

On save, `iedit_save_internally()` first confirms with
`obj_uid_matches(OLC_IEDIT_UID(d), OLC_IEDIT_OBJ(d))` that the object it
started on is still registered at that address (commit `aeeb58d`). This
comparison does not dereference the pointer, so it rejects both a freed
object and a new object allocated at the same address. It then replaces the
instance's strings (freeing the old ones only if they are not the
prototype's), extra descriptions, type, weight, cost, rent, timer, level,
extra, wear and affect bits, the `affected[]` applies and the values, and
sets `PLR_CRASH` on a player who carries or wears it, directly or through
containers. `iedit_replace_values()` copies values only if the types match,
but the type has already been copied, so that guard never refuses.

Two consequences matter to maintainers:

- The fields are written while the object may be worn, carried or inside a
  container, without taking it out and putting it back through the handler.
  `equip_char()` and `unequip_char()` apply and remove applies, affect bits
  and light; `obj_to_char()`, `obj_from_char()`, `obj_to_obj()` and
  `obj_from_obj()` add and subtract weight. Changing those fields in place
  leaves the wearer's statistics, the carrier's carried weight, container
  weights and the room light count wrong when the object later moves.
- When the save is refused because the object is gone, `iedit_parse()`
  still logs `OLC_IEDIT_OBJ(d)->short_description` through the dangling
  pointer and still prints "Object instance saved."

Saving the object's prototype in `oedit` later overwrites every instance,
including iedited ones, and discards their custom strings (leaking them).

### medit (mobile prototypes)

| | |
| --- | --- |
| Command | `medit <vnum>`, `medit save [zone]`; `LVL_BUILDER`. |
| State and scratch | `CON_MEDIT`; `OLC_MOB`, `OLC_SCRIPT`. |
| Live data changed | `mob_proto[]`, `mob_index[]` and the strings and triggers of live instances, through `genmob.c:add_mobile()` / `delete_mobile()`. |
| Writer | `genmob.c:save_mobiles()` (`medit_save_to_disk()` is passed a zone **vnum** and converts it back with `real_zone()`). |

**Setup.** `medit_setup_existing()` fills a new mobile with
`genmob.c:copy_mobile()`, a struct copy of the prototype plus duplicated
name, title and descriptions. Those strings are all the scratch copy owns:
`affected` and `SCRIPT` are NULL in a prototype, and `player_specials`
points at the shared `dummy_mob`. `copy_mobile()` also runs
`check_mobile_strings()` on the **prototype**, so merely opening the editor
gives a NULL name or description in the live prototype the text "An
undefined string." `medit_setup_new()` uses `init_mobile()` defaults,
placeholder strings and `nr` = `NOBODY`. The scratch copy's `nr` is
load-bearing: `free_mobile()` compares its strings with `mob_proto[nr]`,
`check_mobile_strings()` reads `mob_index[nr]`, and an update copies it
into the prototype (see Copy). Inserts renumber it; deletes do not.

**Menus.** Sex, keywords, short and long descriptions (one line each),
detailed description (string editor, `MAX_MOB_DESC`), positions, attack
type, NPC flags (`MOB_ISNPC` and `MOB_NOTDEADYET` are not offered), affect
flags (charm, poison and sleep are cleared after every toggle), a stats
submenu (abilities and saving throws only with `CONFIG_MEDIT_ADVANCED`),
`S` trigger list, `W` copy, `X` delete, `Q` quit. A prototype keeps its
hit-point dice in `points.hit`, `points.mana` and `points.move` (count,
size, bonus), from which `db.c:read_mobile()` rolls each instance's hit
points. Every answered prompt marks the session modified.

**Save.** `medit_save_internally()` decides insert or update with
`real_mobile(OLC_NUM(d))` and calls `add_mobile()`. The old prototype
trigger list leaks as in `redit`, and `OLC_SCRIPT` becomes the
prototype's list. Every live instance then has its running script
extracted, its private trigger list replaced by a copy of the new one and
`assign_triggers()` called; unlike in `oedit`, the extraction really
happens, because `add_mobile()` leaves instance scripts alone. After an
insert, other sessions are renumbered: `CON_MEDIT` scratch `nr` (the
saving session's own included) and `CON_SEDIT` keepers, skipping
`NOBODY`, and argument 1 of `'M'` commands in `CON_ZEDIT` sessions.
"Mobile saved to disk." is printed whatever `save_mobiles()` returned.

**Copy (`W`).** `medit_setup_existing()` runs for the source on top of the
current scratch copy, leaking it and its `OLC_SCRIPT`, and leaves `nr`
naming the **source**. Saved under a new vnum this is harmless, because the
insert sets `nr`. Saved over an existing vnum, `add_mobile()`'s struct copy
writes the source's rnum into the edited prototype's `nr`, and
`read_mobile()` copies `nr` into every mobile loaded afterwards: they
report the source's vnum and run its special procedure, and resets count
them against the edited prototype while extraction decrements the source,
so the edited mobile eventually stops loading and the source over-loads.
Any save does the same after another session deleted a lower mobile; the
stale `nr` is then one too high, or past the end of `mob_index[]`, which
extraction then decrements.

**Delete (`X`).** `delete_mobile(real_mobile(OLC_NUM(d)))`, which refuses
the `NOBODY` of a vnum never saved; see
[genmob.c](#genmobc-mobile-prototypes). The scratch copy is then freed with
`nr` still naming the deleted slot: when that was the highest rnum,
`free_mobile()` reads one element past the shrunken `mob_proto[]`.
`OLC_SCRIPT` leaks and other sessions are not renumbered. The "should
never get here" branches (`MEDIT_D_DESC`, `default`) call `cleanup_olc()`
and fall through to `OLC_VAL(d) = TRUE`, dereferencing the NULL workspace.

### zedit (zones and reset commands)

| | |
| --- | --- |
| Command | `zedit [room vnum]`, `zedit save [zone]`; `LVL_BUILDER`. `zedit new <zone> <bottom room> <top room>`; `LVL_IMPL`. No argument edits the builder's current room. |
| State and scratch | `CON_ZEDIT`; `OLC_ZONE`, a private `struct zone_data` holding a copy of the zone header and of **one room's** reset commands. `OLC_NUM` is the room vnum, `OLC_VAL` the index of the command being edited. |
| Live data changed | `zone_table[]`: one zone's header and that room's commands; a new entry for `zedit new`. Mobiles and objects in the game are not touched; changed commands act at the zone's next reset. |
| Writer | `genzon.c:save_zone()` (`zedit_save_to_disk()` is a one-line wrapper). |

**Entry.** The number is a **room** vnum: its zone comes from
`real_zone_by_thing()`, permission is checked for that zone and the room
must exist. The same-target guard compares room vnums, so two builders can
edit different rooms of one zone at once; for the save form it compares the
typed zone number with room vnums, so `zedit save 30` is refused while
someone edits room 30. `zedit save [zone]` calls `save_zone()` directly
(see the note under [Entry](#1-entry)).

**Setup.** `zedit_setup()` allocates the scratch zone, duplicates the name
and builders, copies the other header fields, and builds a command list
from an `'S'` terminator plus a struct copy (`add_cmd_to_list()`) of every
live command that belongs to the room by the rule given under
[genzon.c](#genzonc-zones); a `'*'` command (disabled at boot, by a failed
reset or by `delete_room()`) also joins the room before it. The scratch
`number` and `age` fields are reused as "header changed" and "commands
changed" flags, and `Q` asks to save only if one is set. The copy owns its
name, builders and command array, which `cleanup_olc()` frees, but the
`sarg1`/`sarg2` strings of its `'V'` commands belong to `zone_table`.
Nothing on the `zedit` side frees one, so a session never frees a live
string; the price is leaks: a `'V'` deleted or retyped and then saved
loses the old strings, one typed and not saved the new ones.

**Reset commands.** `struct reset_com` (`db.h`) holds the letter, `if_flag`
(run only if the previous command succeeded), three integer arguments, the
zone-file `line` (printed by reset errors) and the two `'V'` strings. The
loader (`db.c:renum_zone_table()`) and the prompts store **rnums** where
the file has vnums; `save_zone()` converts them back when it writes.

| Letter | arg1 | arg2 | arg3 |
| --- | --- | --- | --- |
| `M` load a mobile | mobile rnum | maximum in the world | room rnum |
| `O` load an object | object rnum | maximum | room rnum |
| `G` give to the last mobile | object rnum | maximum | unused |
| `E` equip the last mobile | object rnum | maximum | wear position (typed 1-based) |
| `P` put in an object | object rnum | maximum | container object rnum |
| `D` set a door | room rnum | direction | 0 open, 1 closed, 2 locked |
| `R` remove an object | room rnum | object rnum | unused |
| `T` attach a trigger | 0 mobile, 1 object, 2 room | trigger rnum | room rnum |
| `V` set a global | 0 mobile, 1 object, 2 room | context (`reset_zone()` uses it only for room globals and passes the room rnum otherwise) | room rnum |

Typed vnums pass through `real_mobile()`, `real_object()` or
`real_trigger()` and are asked again until they exist; a maximum above 100
(`MAX_DUPLICATES`) is cut silently. The room is never asked: it is
`real_room(OLC_NUM(d))` when the command is typed.

**Commands.** `N` asks for a position (not for the first command) and
inserts an `'N'` command made by `genzon.c:new_command()`, which sets
only the letter: the if-flag, the three arguments and the two `'V'`
string pointers hold whatever was on the stack until the prompts fill
them in (commit `2f25691`, after `aeeb58d`, starts the command blank).
`E` re-enters a command from its type, and `D` deletes one. An insert or delete builds a new array, so
the editor keeps an index (`OLC_VAL(d)`), never a pointer. The prompts run
type, if-flag (only above position 0; `'T'` and `'V'` there get 1
unasked), then the arguments the type needs and, for `'V'`, the global's
name and value. Each repeats until answered validly, so a started command
must be finished (`A` at the "change" prompt backs out before a command is
chosen), and fields the new type does not ask for keep their old values. A
global name with a space, or a name or value over 79 characters, reloads
differently: `db.c:load_zones()` reads a one-word name and at most 79
characters of each.

**Menus.** `1` builders (what `can_edit_zone()` matches), `Z` name, `L`
lifespan (0-240 minutes), `R` reset mode (0 never, 1 when no player is in
the zone, 2 always), `F` flags (any, including `ZONE_NOBUILD`, which then
shuts every builder out until `zunlock`), `M` the level range `areas`
shows. `B` and `T`, the room range, are `LVL_IMPL` only, clamped silently
between the neighbouring zones (at most 32000 for the last zone), and move
nothing: whatever is left outside the range stays in memory but is no
longer written, because every zone writer selects by vnum range.

**Save.** `zedit_save_internally()` looks up `real_room(OLC_NUM(d))`; if
the room was deleted meanwhile it logs a SYSERR and returns, and the caller
still announces the save, logs the edit and, with `CONFIG_OLC_SAVE`, writes
the unchanged zone. Otherwise it removes the room's live commands with
`remove_room_zone_commands()` and inserts each scratch command at **its
own scratch index**, so every save, even of a header change alone, moves
the room's commands to the front of the zone's list and file. It refuses a
`'G'` or `'E'` unless an `'M'` came first and a `'P'` unless an `'O'` did;
any other letter clears both. That rule loses data in two ways:

- It is stricter than `reset_zone()`, which gives and equips to the last
  mobile loaded whatever came between and finds a `'P'` container anywhere
  in the world. Applied to the shipped zone files, the rule finds 75
  commands (72 `'P'`, 3 `'E'`) in 22 rooms of 12 zones that are deleted
  the first time their room is saved: room 4030 of zone 40 gives its snake a goblin's
  carcass with coins in it, and saving that room deletes the coins.
- A refused command still uses up its index, so each later insert lands
  one slot further down and a command of another room is pulled into the
  gap. The `'G'`, `'E'` and `'P'` that followed that command now
  sit behind this room's later commands: the next save claims them for
  this room, and if this room loads a mobile after the gap, the next reset
  gives or equips their objects to that mobile. If more commands are
  refused than other commands remain in the zone, `add_cmd_to_list()`
  silently drops the rest.

A changed header is copied whole. It is a snapshot from session start, so
it undoes header changes made since by another `zedit` session (last
writer wins) or by `zlock`, whose `ZONE_NOBUILD` it clears again. The zone
goes on the save list and, with `CONFIG_OLC_SAVE`, is written at once. No
table changes size, so no session is renumbered.

**Stale rnums.** Other editors renumber the scratch list when they insert
a room, object or mobile (see
[Renumbering](#renumbering-on-insert-and-delete) for the gaps), never on a
delete. `trigedit` renumbers no `'T'` command, live or scratch: after it
inserts a trigger, every `'T'` for a higher trigger vnum names the trigger
below it, and `save_zone()` writes that. `zedit_disp_menu()` indexes the
prototype and trigger tables with scratch rnums unchecked.

**New zones.** `zedit new` refuses negative room numbers, then
`zedit_new_zone()` calls `create_new_zone()` (see
[genzon.c](#genzonc-zones)) and adds one to every `OLC_ZNUM(d)` at or above
the new rnum in `CON_REDIT` (with its scratch room's `zone`), `CON_ZEDIT`,
`CON_MEDIT`, `CON_SEDIT`, `CON_OEDIT`, `CON_TRIGEDIT` and `CON_QEDIT`
sessions; no other state keeps a zone rnum there. The reset queue
(`reset_q`, `db.c:zone_update()`) also holds zone rnums and is not
renumbered: the entry of a zone queued at or above the new rnum now names
another zone, which is reset in its place, and the queued zone stays at age
`ZO_DEAD`, never resetting until `zreset` or a reboot. No session opens.
The new `.wld` file holds a room at `bottom` that is not loaded into
`world[]`, so until a reboot `zedit` on it answers "That room does not
exist." and `redit` has to create it.

### sedit (shops)

| | |
| --- | --- |
| Command | `sedit <vnum>`, `sedit save [zone]`; `LVL_BUILDER`. The shop vnum must lie in a zone's room range. |
| State and scratch | `CON_SEDIT`; `OLC_SHOP`. No trigger list and no string editor. |
| Live data changed | `shop_index[]` through `genshp.c:add_shop()`, and `mob_index[].func` of every mobile chosen as keeper, at the moment it is chosen. |
| Writer | `genshp.c:save_shops()`. |

**Setup.** `sedit_setup_existing()` allocates a zeroed shop and fills it
with `copy_shop(..., FALSE)`, a deep copy: the scratch copy owns its three
lists and seven messages and borrows nothing. `sedit_setup_new()` sets
keeper `NOBODY`, closing hour 28, both profit factors 1.0, seven default
messages and three lists holding only their terminators. The lists are
never NULL, which `copy_shop()` and the list helpers rely on. Products are
object **rnums**, rooms are room **vnums** and the keeper is a mob **rnum**
(see [genshp.c and genqst.c](#genshpc-and-genqstc)).

**Menus.** Keeper (a mob vnum, or -1 for none), opening and closing hours
(clamped to 0..28), buy and sell profit factors (any finite number, zero
and negatives included), seven one-line messages (`modify_shop_string()`
puts "%s " before text that does not start with `%`; `shop.c` expands them
as templates, never as printf formats), no-trade and shop flags (a number
above the last flag toggles the last flag), `R` rooms, `P` products, `T`
accepted item types and namelists, `W` copy, `Q` quit. There is no delete:
GenOLC has no `delete_shop()`. Every answered prompt marks the session
modified, and between the type menu and the namelist prompt `OLC_VAL(d)`
holds the chosen item type instead. Object rnum 0 can never be added as a
product (`if (i > 0)`). The long room list (`R`, shown again after every
room change) rewrites a room that no longer exists to vnum 0, the Void,
and a save keeps that. Menus index the mobile and object tables with the
scratch rnums unchecked.

**The keeper.** Choosing a keeper changes live data at once, with no
permission check on the mobile's zone: `S_FUNC` takes the mobile's special
procedure (NULL if it is already `shop_keeper`) and its `mob_index[].func`
becomes `shop_keeper`. Nothing undoes this. Quitting without saving, or
choosing another keeper or -1, leaves every mobile tried running
`shop_keeper()`, which finds no shop for it and returns FALSE, so its own
procedure is gone until reboot; the old keeper of a saved shop fares the
same. Re-entering the current keeper sets `S_FUNC` to NULL, dropping the
procedure the shop chained to. A questmaster chosen as keeper has
`questmaster` captured in `S_FUNC`, which can close a loop with
`add_quest()` (see [qedit](#qedit-quests)).

**Save.** `sedit_save_internally()` sets the scratch vnum to `OLC_NUM(d)`
and calls `add_shop()`, which copies every field over the live entry: the
banked gold (`bankAccount`) and sort counter as they were when the editor
opened, so gold banked during the edit is lost, and keeper or product
rnums left stale by a delete in another session. No shop rnum is stored
anywhere, so nothing is renumbered. `cleanup_olc(d, CLEANUP_STRUCTS)`
still frees the whole scratch shop with `free_shop()`, correctly, since
the live entry has its own copies. "Shop saved to disk." is printed
whatever `save_shops()` returned. The keeper uses the new data from the
next command typed in its room, because `shop.c:shop_keeper()` looks its
shop up by keeper rnum on every call.

**Copy (`W`).** `sedit_setup_existing()` runs for the source on top of the
current scratch copy, leaking it with its lists and messages. The copy
keeps the source's keeper and `S_FUNC`, so saving it gives two shops one
keeper, and `shop_keeper()` serves only the one with the lower rnum;
`scopy` does the same. The `default` "should never get here" branch calls
`cleanup_olc()` and then falls through to `OLC_VAL(d) = 1`, using the now
NULL workspace.

### qedit (quests)

| | |
| --- | --- |
| Command | `qedit <vnum>`, `qedit save [zone]`; `LVL_BUILDER`. The quest vnum must lie in a zone's room range. |
| State and scratch | `CON_QEDIT`; `OLC_QUEST`. |
| Live data changed | `aquest_table[]` through `genqst.c:add_quest()` / `delete_quest()`, and the questmaster's `mob_index[].func`. |
| Writer | `genqst.c:save_quests()`. |

**Entry.** `do_oasis_qedit()` lacks the first entry check: it never tests
`IS_NPC()`, `ch->desc` or `CON_PLAYING`. Forcing a linkless builder into
`qedit` (with `force`, or a DG `%force%`) dereferences the NULL descriptor
and crashes the server. A builder forced in from inside another editor
(only `force` can do that) has that workspace freed with a plain `free()`,
leaking its scratch data; if `qedit` then stops early (no zone, no
permission, or the `save` form), the descriptor is left in the old
editor's state with `d->olc` NULL, and the next line typed crashes the
server. NPCs never get here: `command_interpreter()` refuses them
immortal commands.

**Setup.** `qedit_setup_existing()` fills a new quest with
`copy_quest(..., FALSE)`: every field, with the five strings duplicated by
`str_udup()`, so none is NULL (an empty one becomes "undefined", which the
menu shows as "Nothing" for the three messages). `qedit_setup_new()` sets type `AQ_UNDEFINED`,
questmaster `NOBODY`, target, prerequisite, reward object and quest links
`NOTHING`, levels 0 to `LVL_IMPL`, no time limit (-1), quantity 1 and
placeholder strings. Every reference in a quest is a **vnum**, so the
scratch copy never needs renumbering.

**Menus.** Name and description (one line each, cut at 39 and 74 bytes),
acceptance, completion and quit messages (string editor,
`MAX_QUEST_MSG`), flags, type ("Return object" also asks for the mobile),
questmaster, target, quantity, points, level limits, prerequisite, gold,
experience and object rewards, time limit, quest links, `X` delete, `Q`
quit; there is no `W`. Vnums are checked with the `real_*()` lookups (-1
means none) after being cut to 16 bits, so 68537 passes as 3001; the
target is stored unchecked whatever the type. Quantity, points, gold,
experience and time limit are clamped; level limits are refused outside
0..`LVL_IMPL` or when they cross. The flag menu does `flags ^= number`,
using the menu number as the bit, which is right only while
`AQ_REPEATABLE` is the only flag. Every answered prompt marks the session
modified.

**Save.** `add_quest()` deep-copies the scratch quest into
`aquest_table[]` (in place, or inserted in vnum order), so the editor
still owns its five strings, but the confirm path ends with
`cleanup_olc(d, CLEANUP_STRUCTS)`, which frees only the struct: every
successful save leaks them. `add_quest()` then gives the questmaster
`questmaster()` as its special procedure, keeping any other one in
`QST_FUNC`. A replaced questmaster keeps `questmaster()`, its own
procedure gone until reboot; if the new one has none, `QST_FUNC` still
holds the old one's, which the new questmaster now runs. If the mobile is
a keeper whose shop chains to `questmaster` (see [sedit](#sedit-shops)),
`QST_FUNC` becomes `shop_keeper`, and the two call each other until the
stack overflows the next time anyone types a command in the mobile's
room. "Quest %d saved to disk." is printed whatever `save_quests()`
returned.

**Delete (`X`).** `delete_quest(real_quest(OLC_NUM(d)))`. For a quest
never saved, `real_quest()` gives `NOTHING`, and `delete_quest()` reads
`aquest_table[65535]` before its range check (the table is NULL when no
quest exists); the editor then reports failure and saves the quest file
anyway. For a saved quest see [genshp.c and genqst.c](#genshpc-and-genqstc).
Players on a deleted quest keep its vnum, as do quests that link to it:
`quest.c` mostly treats a missing quest as none, but
`act.informative.c:do_score()` and the links shown by `quest.c:quest_list()`
pass `real_quest()`'s `NOTHING` straight to `QST_NAME()`. The `default`
"should never get here" branch has the same use-after-cleanup as `medit`'s.

### trigedit (triggers)

`trigedit <vnum>` (`LVL_BUILDER`, `dg_olc.c:do_oasis_trigedit()`) follows
the standard entry checks (same-target guard, `real_zone_by_thing()`,
`can_edit_zone()`), then `trigedit_setup_new()` or
`trigedit_setup_existing()` fills `OLC_TRIG` with a scratch trigger and
`OLC_STORAGE` with the script text, and the state becomes `CON_TRIGEDIT`.
`trigedit_parse()` handles the menus; the script body goes through the
string editor, whose `/f` command formats it (`format_script()`). Saving
calls `trigedit_save()`, which rebuilds the prototype's command list,
patches live triggers, inserts new prototypes into `trig_index[]` (shifting
trigger rnums), writes `lib/world/trg/<zone>.trg` and the trigger index, and
the session ends with `cleanup_olc(d, CLEANUP_ALL)`, which frees `OLC_TRIG`
and `OLC_STORAGE`. The details, including what happens to running triggers
and how trigger rnums are renumbered, are in
[dg-scripts.md](dg-scripts.md#trigger-olc).

The same file holds the **trigger-list editor** used by `medit`, `oedit`
and `redit`: `dg_olc_script_copy()` copies the entity's `proto_script`
(a linked list of trigger vnums) into `OLC_SCRIPT(d)` at setup,
`dg_script_menu()` and `dg_script_edit_parse()` add and remove entries
(adding checks `real_trigger()`; each change increments `OLC_VAL(d)`), and
the editor's save hands the list to the prototype.

### cedit (game configuration)

| | |
| --- | --- |
| Command | `cedit`, `cedit save`; `LVL_IMPL`, checked again in `do_oasis_cedit()`. |
| State and scratch | `CON_CEDIT`; `OLC_CONFIG`, a whole `struct config_data`. `OLC_VAL` is unused, so `Q` always asks "Do you wish to save your changes?". |
| Live data changed | `config_info` (`db.c`), which the game reads through the `CONFIG_*` macros of `utils.h`; room special procedures, through `reassign_rooms()`. |
| Writer | `cedit.c:save_config()`, which writes `CONFIG_CONFFILE` (normally `lib/etc/config`). `cedit_save_to_disk()` wraps it and drops its result. |

**Where the values live.** The globals in `config.c` (declared in
`config.h`) are only defaults. At boot, `db.c:load_config()` calls
`load_default_config()` to copy them into `config_info`, `strdup()`ing the
strings, and then applies `lib/etc/config` on top. From then on
`config_info` is the only live copy.

**Setup.** `do_oasis_cedit()` returns silently for an NPC, a missing
descriptor or a state other than `CON_PLAYING`. It has no same-target guard
and does not check for a leftover workspace before `CREATE(d->olc)`.
`cedit_setup()` copies every field except `CONFFILE` and duplicates the ten
strings: `OK`, `HUH`, `NOPERSON` and `NOEFFECT` with `str_udup()` (an empty
one becomes "undefined"), the other six with `strdup()` or NULL.

**Menus.** Game play, crash save and rent, room numbers, operation, and
autowiz. Yes/no fields flip with `TOGGLE_VAR()`. Numbers go through
`tb_parse_int_or_zero()` and are mostly stored unchecked. The port, player
limit, file-size and password prompts even take an empty line as 0, and a
saved `max_playing` of 0 makes `comm.c:new_descriptor()` turn away every
connection. A room number must exist (`real_room()`), but it is cut to 16
bits before the check and the store, so 68537 is accepted as room 3001.
The four messages take one line each and get "\r\n" appended
(`str_udupnl()`). Changing `DFLT_IP`, `DFLT_DIR` or `LOGNAME` leaks the old
scratch string. `MENU`, `WELC_MESSG` and `START_MESSG` go to the string
editor with a 512-byte limit.

**Save.** `Y` runs `cedit_save_internally()`, which copies **every** field
back, not only the changed ones, freeing and re-duplicating each live
string. If `dts_are_dumps` changed, `reassign_rooms()` sets `func` to NULL
on rooms 0 to `top_of_world - 1`, missing the last room (`top_of_world` is
inclusive), and calls `spec_assign.c:assign_rooms()` to put back the pet
shops and, when the flag is on, `dump` on every `ROOM_DEATH` room. The
closing `add_to_save_list(NOWHERE, SL_CFG)` is always refused, so the change
never shows in `olc`, and `saveall` and shutdown never write it.
`cedit_parse()` then calls `cleanup_olc(d, CLEANUP_CONFIG)` and writes the
file only if `CONFIG_AUTO_SAVE` is set. That is `csd.auto_save`, the player
crash-save toggle, not `CONFIG_OLC_SAVE`. Otherwise the implementor is told
"saved to memory", and only `cedit save` writes the file. Neither path
checks `save_config()`'s result before reporting success.

**When a change takes effect.** Most settings are read where they are used.
The port, IP address, data directory and log file are used only at boot
(`comm.c:main()`, `get_bind_addr()`), and so is `use_new_socials`
(`db.c:boot_social_messages()`), although turning it off disables `aedit`
at once. `db.c:check_start_rooms()` resolves the three start rooms into
`r_mortal_start_room`, `r_immort_start_room` and `r_frozen_start_room` at
boot and never again, so a new start room is used only after a reboot.

**The file.** `save_config()` builds the whole file from `config_info`, not
from the scratch copy, so `cedit save` writes whatever is live. It writes
`<CONFFILE>.tmp`, checks the flush and close, installs the file with
`tb_replace_file_checked()` and returns `FALSE` on any failure. Comments
and unknown lines in the old file are not kept. Two settings do not survive
a reboot: the writer emits `disp_closed_doors` where `load_config()` reads
`display_closed_doors`, and it never writes `min_rent_cost`. Both return to
their `config.c` defaults at every boot.

**Cleanup.** `cleanup_olc()` frees `OLC_CONFIG` only inside its `OLC_ROOM`
branch (see [Cleanup](#6-cleanup)), so every `cedit` session, saved or not,
leaks the scratch struct and its ten strings.
`oasis_delete.c:free_strings(..., OASIS_CFG)` frees exactly those strings,
without clearing the pointers; in practice it runs only at shutdown, on
`config_info`. On link loss `close_socket()` does nothing for `CON_CEDIT`:
nothing is applied and the workspace leaks.

**Concurrency.** Two implementors can edit at once. Because a save copies
every field, the later save silently undoes the earlier one. It also
reverts `toggle slownameserver` and `toggle trackthru`
(`act.informative.c:do_toggle()`), which change `config_info` directly.

**Command-line pointers.** Given `-o <file>`, `comm.c:main()` points
`CONFIG_LOGNAME` into `argv`. The next `cedit` save, and the shutdown call
to `free_strings()`, pass that pointer to `free()`. That is undefined
behaviour, and glibc normally aborts the server.

### aedit (socials)

| | |
| --- | --- |
| Command | `aedit <social>`, `aedit save`; `LVL_GOD`, plus `can_edit_zone(ch, AEDIT_PERMISSION)` (OLC zone `all` or `aedit`, or `LVL_GRGOD`). Refused while `use_new_socials` is off. |
| State and scratch | `CON_AEDIT`; `OLC_ACTION` (a `struct social_messg`), `OLC_STORAGE` (the word typed), and `OLC_ZNUM` used as an **index into `soc_mess_list[]`**, or `top_of_socialt + 1` for a new social. |
| Live data changed | `soc_mess_list[]` and `top_of_socialt` (an inclusive index); `complete_cmd_info[]` and `cmd_sort_info[]`, which every save rebuilds. |
| Writer | `aedit.c:aedit_save_to_disk()` writes `lib/misc/socials.new` (`SOCMESS_FILE_NEW`) on every save. |

**Entry.** `do_oasis_aedit()` returns silently for an NPC, a missing
descriptor or a state other than `CON_PLAYING`, then checks the socials
format and permission. Next comes the **single-editor rule**: if any
descriptor is in `CON_AEDIT`, it refuses, `aedit save` included. The rule
is what keeps `OLC_ZNUM(d)` valid, because every save reallocates or
re-sorts `soc_mess_list[]`. The word is matched against social names with
`is_abbrev()`, and `N` at "Do you wish to edit ...?" moves to the next
match. With no match left, a word that `aedit_find_command()` finds among
the commands is refused (silently, after `N`); any other word is offered
as a new social.

**Setup and menus.** `aedit_setup_existing()` `strdup()`s every string of
the live entry; NULL stays NULL. `aedit_setup_new()` uses the word as both
command and sort key and gives the two no-argument messages "This action is
unfinished.". Command and sort key must be one word (otherwise the menu
returns without a message), and nothing checks that they are unique.
Positions outside `POS_DEAD`-`POS_STANDING` are refused (commit `2eec6ee`),
but the level test `(i < 0) && (i > LVL_IMPL)` is never true, so any level
is stored. The thirteen messages are one line each, and an empty line makes
one NULL. They skip `genolc_checkstring()`, so `@` colour codes stay
literal until the next boot.

**Save.** `Y` runs `aedit_save_internally()`. A new social is appended to a
reallocated array (old entries struct-copied, old array freed). An existing
one is replaced after `act.social.c:free_action()` frees the live entry's
strings. Either way the table now **owns the scratch strings**, so the
session ends with `cleanup_olc(d, CLEANUP_STRUCTS)`. The save then:

- calls `create_command_list()`, which frees `complete_cmd_info[]`,
  re-sorts `soc_mess_list[]` by sort key and rebuilds `complete_cmd_info[]`.
  Each social's entry points at the social's own `command` and `sort_as`
  strings, and the social's `act_nr` becomes its new command number;
- calls `interpreter.c:sort_commands()`, which allocates a new
  `cmd_sort_info[]` and leaks the old one;
- writes the file at once, whatever `CONFIG_OLC_SAVE` says. "Action saved
  to disk." is printed even when the write failed.

**Command numbers move.** Adding a social, or changing a sort key, shifts
every later command in `complete_cmd_info[]`. `do_action()` follows because
it searches `act_nr`, but numbers cached from `find_command()` do not:
`cmd_slap` and `cmd_puke` (`shop.c`, set at boot), `snarl_cmd` (`mobact.c`)
and `spit_social` (`spec_procs.c`). After such a save those calls perform
whichever social now has the old number, or fail with "That action is not
supported.".

**The file.** `aedit_save_to_disk()` writes the whole table, in sorted
order, to `socials.new.tmp`, checks the flush and close, renames it with
`tb_replace_file_checked()` and only then drops the save-list entry. Each
social is a line `~<command> <sort_as> <hide> <min_char_pos> <min_vict_pos>
<min_level>`, thirteen message lines (`#` for NULL) and a blank line; `$`
ends the file. A failure reaches only the log, and `save_all()` cannot
write socials, so only `aedit save` retries. On reading,
`db.c:fread_action()` treats a line starting with `#` as NULL, so a message
the builder began with `#` disappears at the next boot, and
`boot_social_messages()` does not range-check positions or level.

**astat and link loss.** `do_astat()` (level 0; NPCs ignored) shows the
first social the argument abbreviates. Like `aedit_disp_menu()` it indexes
`position_types[]` with the stored positions, so a bad position read from
the file is read past that array. On link loss `close_socket()` calls
`cleanup_olc(d, CLEANUP_ALL)`, which frees the scratch social, its strings
and `OLC_STORAGE`. Nothing is applied.

### hedit (help)

| | |
| --- | --- |
| Command | `hedit <keyword>`, `hedit save`; `LVL_GOD`, plus `can_edit_zone(ch, HEDIT_PERMISSION)` (OLC zone `all` or `hedit`, or `LVL_GRGOD`). `helpcheck` (`LVL_GOD`) and `hindex` (everyone) only list. |
| State and scratch | `CON_HEDIT`; `OLC_HELP` (a `struct help_index_element`), `OLC_STORAGE` (the word typed), and `OLC_ZNUM` used as an **index into `help_table[]`**, or `NOWHERE` for a new entry. |
| Live data changed | `help_table[]` and `top_of_helpt`, which is a **count**, not an inclusive index. A successful write frees the whole table and loads it again. |
| Writer | `hedit.c:hedit_save_to_disk()` writes `lib/text/help/help.hlp` on every save. |

**How help is held.** `db.c:load_help()` keeps each entry's keyword line as
the first line of its text and makes one row per keyword. Each row has its
own copy of one keyword. All rows of an entry share one `entry` string,
owned by the first row (`duplicate` 0); the others have `duplicate` 1, 2
and so on. `index_boot(DB_BOOT_HLP)` sorts the rows by keyword for
`act.informative.c:search_help()`. `free_help_table()` frees every keyword
but only the owners' entries.

**Entry.** `do_oasis_hedit()` returns silently for an NPC, a missing
descriptor or a state other than `CON_PLAYING`, and checks permission.
Then comes the **single-editor rule**: if any descriptor is in `CON_HEDIT`,
it refuses, `hedit save` included, because `OLC_ZNUM` indexes a table that
every save rebuilds. The word is looked up with `search_help(word,
LVL_IMPL)`. A duplicate row is replaced by its owner, and a miss offers a
new entry (commit `d532d66` stopped that path from reading
`help_table[NOWHERE]`). `search_help()` treats `top_of_helpt` as an
inclusive bound, so a word that sorts after every keyword reads one row
past the table, here and in `help`. `N` at "Do you wish to edit ...?" walks
on with `is_abbrev()` to the next match, and that walk does **not** move to
the owner row.

**Setup and menus.** `hedit_setup_existing()` copies the keyword and entry
with `str_udup()`, plus `duplicate` and `min_level`. `hedit_setup_new()`
starts the text with the line "KEYWORDS". The menu offers `1` (the text, in
the string editor, at most 4096 bytes), `2` (minimum level, 0 to
`LVL_IMPL`) and `Q`; `HEDIT_KEYWORDS` is unreachable. The keywords that
reach the file are the first line of the text, so a new entry is filed
under "KEYWORDS" unless that line is replaced. After `/c` then `/s` the
entry is NULL. Choosing `1` again then hands the literal `oldtext = ""` to
`string_write()` as the abort copy, and saving or aborting that text passes
the literal to `free()`, which aborts the server.

**Save.** `Y` prints "Help saved to disk." first and then runs
`hedit_save_internally()`. An index at or past `top_of_helpt` is reported
and treated as new (commit `736ee87`). A new entry is appended at the
**end** of a reallocated table, out of sort order. An existing row is
overwritten by struct copy without freeing its old keyword (or, for an
owner, its old entry, which the duplicate rows still point to). The table
now owns the scratch strings, so the session ends with
`cleanup_olc(d, CLEANUP_STRUCTS)`. The file is written at once, whatever
`CONFIG_OLC_SAVE` says.

**The file and the reload.** `hedit_save_to_disk()` writes `help.hlp.tmp`
from every row whose `duplicate` is 0: the text with carriage returns
stripped and colour codes converted back, then `#<min_level>`, and `$~` at
the end. It checks the flush and close and renames the file. Only then does
it drop the save-list entry, call `free_help_table()` and run
`index_boot(DB_BOOT_HLP)`. Consequences:

- A row reached by the `N` walk can be a duplicate. The writer skips it, and
  the reload discards the edit the builder was told was saved.
- The whole table goes into `help.hlp`, so entries from any other file
  listed in `lib/text/help/index` come back twice. The stock index lists
  only `help.hlp`.
- The reload is the boot loader: a file that cannot be opened or parsed
  means `exit(1)`.
- Every pointer into the old table dies. `do_help()` pages
  `help_table[mid].entry` with `page_string(..., 0)`, which keeps pointers
  into it between input lines, so a player paging a help text during a
  save reads freed memory at the next page. `reload all` and `reload xhelp`
  (`db.c:do_reboot()`) free and reload the table the same way, without
  checking for an open `hedit` session.
- If the write fails nothing is reloaded. The change stays in memory, a
  new entry stays at the unsorted end where the binary search can miss it,
  and `save_all()` cannot write help, so only `hedit save` retries.

**Link loss.** `close_socket()` calls `cleanup_olc(d, CLEANUP_ALL)`.
`db.c:free_help()` frees the scratch keyword and, unless `duplicate` is
set, the scratch entry, which therefore leaks for a duplicate row.

**helpcheck and hindex.** `do_helpcheck()` lists the commands that
`search_help()` cannot find. `do_hindex()` lists the keywords the argument
abbreviates or contains, up to the player's level, in bounded buffers, and
says when the listing was cut. Both page a copy of their text.

### tedit (text files)

| | |
| --- | --- |
| Command | `tedit [file]`; `LVL_GOD`, and each file has its own level (`LVL_GRGOD` or `LVL_IMPL`) and size limit (2400 or 8192 bytes) in the table in `do_tedit()`. With no argument it lists what the player may edit. The name may be abbreviated; the first match in table order wins. |
| State and scratch | `CON_TEDIT`; `OLC_STORAGE` holds a `strdup()` of the file name. There is **no scratch copy** of the text. |
| Live data changed | The text global itself: `credits`, `news`, `motd`, `imotd`, `GREETINGS`, `help` and `ihelp` (the screens `help` shows with no argument), `info`, `background`, `handbook`, `policies`, `wizlist` or `immlist`. |
| Writer | `tedit.c:tedit_string_cleanup()`, which rewrites the file in place. |

**Setup.** `do_tedit()` checks only that `ch->desc` exists. It does not
refuse an NPC or a descriptor outside `CON_PLAYING`, so an immortal forced
to `tedit` from inside another editor loses that session: the old
workspace is released with a plain `free()` after the "Player already had
olc structure" log line, leaking its scratch data. The command prints the
current text and calls `string_write()` with `d->str` set to the **address
of the global** and a `strdup()` of the text as the abort copy. `tedit` has
no parse function; every line goes to `string_add()`.

**Editing the live buffer.** Because the string editor works on the global
itself:

- Each line is appended to the text the game is showing, so readers see
  the half-finished text, and after `/c` the global is NULL until a line is
  typed.
- The editor reallocates the buffer as it grows and frees it on `/c` and
  `/a`. `do_gen_ps()` and `do_help()` page these texts with
  `page_string(..., 0)`, which keeps pointers into the buffer between input
  lines, so a player paging the text meanwhile reads freed memory.
- Nothing stops two sessions on one file. They append to the same buffer,
  each keeps its own abort copy, and whichever ends last decides the text.

**Save.** `/s` (or `@`) makes `string_add()` free the abort copy and turn an
empty text into "Nothing.\r\n", but it leaves a NULL text NULL.
`tedit_string_cleanup()` then opens the file with `fopen(..., "w")`, which
truncates it in place with no temporary file, strips the carriage returns
from the **live** buffer and writes it. Write and close errors are only
logged, and "Saved." is printed anyway. If `fopen()` fails, the only trace
is a `mudlog` line at `LVL_IMPL`, and memory and disk now differ. Saving
`news` or `motd` sets `newsmod` or `motdmod`, which adds "(news)" or
"(motd)" to the prompt of players who have not read it since. Every exit
ends with `cleanup_olc(d, CLEANUP_ALL)`, which frees `OLC_STORAGE`.

**Afterwards.** Text loaded by `db.c:file_to_string()` ends its lines with
"\r\n"; after a save the global has bare "\n" until it is next loaded
(`reload` or reboot). A NULL global is written as an empty file and stays
NULL. For `GREETINGS` that crashes the server at the next connection when
protocol negotiation is off (`comm.c:new_descriptor()` calls
`strlen(GREETINGS)`); elsewhere glibc prints "(null)".

**Link loss.** `close_socket()` calls `cleanup_olc(d, CLEANUP_ALL)`. The
partly edited text stays in the global, nothing restores the abort copy
(which leaks), and the file is not written.

### prefedit (preferences)

| | |
| --- | --- |
| Command | `prefedit` (level 0, one's own preferences); `prefedit <player>` (`LVL_IMPL`, any player `get_player_vis()` finds in the game). |
| State and scratch | `CON_PREFEDIT`; `OLC_PREFS`, a `struct prefs_data` (`prefedit.h`): copies of the `PRF` flag array, wimp level, page length and screen width, plus `ch`, a **borrowed** pointer to the character being edited. No strings. |
| Live data changed | The edited character's `PRF_FLAGS()`, `GET_WIMP_LEV()`, `GET_PAGE_LENGTH()` and `GET_SCREEN_WIDTH()`; also the editor's own `d->pProtocol` settings. |
| Writer | `save_char()` on the edited character, from `prefedit_save_to_char()`. |

**Entry.** `do_oasis_prefedit()` skips the usual entry checks: it never
tests that `ch->desc` exists or is `CON_PLAYING`, and goes straight to
`ch->desc->olc`. A linkless player made to run the command (an immortal's
`force`, or a DG `mforce`, which accepts linkless mortals) crashes the
server. A builder forced into it from another editor loses that session to
a plain `free()` of the old workspace. A mobile is refused as the victim,
and so is anyone another `CON_PREFEDIT` session is already editing, so a
player and an implementor cannot edit the same preferences at once.
`prefedit_setup()` allocates `OLC_PREFS` and copies the values.

**The borrowed pointer.** Every menu dereferences `OLC_PREFS(d)->ch`
(`GET_NAME()`, `GET_LEVEL()`, `->desc`). It stays valid only because
`db.c:free_char()` calls `prefedit_forget_char()` for every player it
frees, which ends any session editing that character from another
descriptor with `cleanup_olc(d, CLEANUP_ALL)` and "<name> has left the
game; preference edit abandoned." (commit `c5a3600`). New code that frees
a player some other way, or that keeps a `char_data *` in a workspace,
needs the same hook.

**Menus.** Prompt, colour, page length (clamped to 10-60), screen width
(40-120), wimpy (0-500), the toggles menu, `D` restore defaults, and the
immortal flags `1`-`8`, offered by the **edited** character's level. All
of them change only the scratch copy, except the protocol settings `J` to
`R` in the toggles menu, which flip `d->pProtocol` of the editor's own
connection at once: answering `N` does not undo them, nothing saves them,
and an implementor editing someone else changes their own connection.
Wrong at HEAD:

- `prefedit_Restore_Defaults()` tests `PRF_AUTOMAP`, `PRF_AUTOKEY`,
  `PRF_AUTODOOR` and `PRF_VERBOSE` the wrong way round, so those "on"
  defaults are never set. `PRF_NOHASSLE`, `PRF_HOLYLIGHT` and
  `PRF_SHOWVNUMS` are flipped for immortals above `LVL_IMMORT` (one who
  has the flag loses it) and cleared at `LVL_IMMORT` itself.
- An invalid syslog choice shows the **colour** menu and switches to its
  mode, so the next answer sets the colour level.

**Save.** `Q` asks only if `OLC_VAL` is set. `Y` calls
`prefedit_save_to_char()`. If the character still has a descriptor that
`IS_PLAYING()`, it copies the whole flag array and the three numbers into
the character and calls `save_char()`, writing the player file at once.
Otherwise it logs a SYSERR and tells the editor "Unable to save toggles
(...)", and the session ends anyway, so a linkless player's preferences can
never be saved. Because the whole array is copied back, any `PRF` flag the
player changed meanwhile (`toggle`, `afk`) reverts to the snapshot. Every
exit frees `OLC_PREFS` through `cleanup_olc()`.

**Link loss.** `close_socket()` does not clean up `CON_PREFEDIT`: the
workspace leaks and nothing is applied. The closed descriptor leaves
`descriptor_list`, so it no longer blocks anyone from editing.

**prefedit.h.** The header has no include guard and needs `structs.h`
(`PR_ARRAY_MAX`) and `oasis.h` (`OLC_PREFS`) first. Its `PREFEDIT_*` macros
use a variable named `d` from the caller's scope, and `PREFEDIT_GET_CHAR`
falls back to `d->character` when `ch` is NULL.

### msgedit (combat messages)

| | |
| --- | --- |
| Command | `msgedit` lists the slots; `msgedit <slot>` edits one; `LVL_GOD`. The number is an **index into `fight_messages[]`** (0 to `MAX_MESSAGES - 1`, which is 59), not an attack type. A non-numeric argument reads as slot 0. |
| State and scratch | `CON_MSGEDIT`; `OLC_MSG_LIST`, a heap `struct message_list` holding deep copies; `OLC_MSG`, a **cursor** into that list; `OLC_NUM`, the slot. |
| Live data changed | `fight_messages[OLC_NUM(d)]`: its `a_type`, `number_of_attacks` and message list. |
| Writer | `msgedit.c:save_messages_to_disk()` rewrites `lib/misc/messages` (`MESS_FILE`) in place on every save. |

**The table.** At boot `load_messages()` empties the 60 slots and reads
`lib/misc/messages`, skipping blank lines and lines that start with `*`.
Each `M` record is a type line and twelve message lines read with
`db.c:fread_action()`, which turns a line starting with `#` into NULL. A
record goes to the first slot whose `a_type` equals its type or is 0, so
slots are numbered in order of first appearance; records are prepended and
counted in `number_of_attacks`. Because 0 means "empty", type 0 cannot be
stored: the next new type read takes over that slot and its messages.
Almost every read or parse error calls `exit(1)`. `fight.c:skill_message()`
finds a slot by `a_type`, picks a message at random up to
`number_of_attacks`, and treats a count of 0 as missing data.

**Entry.** `do_msgedit()` does not check for an NPC, a missing descriptor
or `CON_PLAYING`; it goes straight to `ch->desc->olc`. A linkless god made
to run it (`force`, or a DG `mforce`) crashes the server, and a god forced
into it from another editor loses that session to a plain `free()`. A second session on
the same slot is refused; sessions on different slots are allowed.
`msgedit_setup()` fills `OLC_MSG_LIST` with `copy_message_list()`, which
copies `a_type` and `number_of_attacks`, `strdup()`s every string and builds
the copy by prepending, so the scratch list is in **reverse** order. An
empty slot gets one blank message, but its count stays 0.

**Menus.** `1` sets the type, clamped to 0-500 without a message. `A` to `L`
are the twelve strings, one line each, through `genolc_checkstring()` and
`delete_doubledollar()`; an empty line stores "", not NULL. `N` moves to
the next message or, at the last, appends a blank one and increments the
count. `P` goes back, `S` saves and `Q` quits. An unknown letter at the
main menu also marks the session modified.

**Save.** This is a copy-edit-copy-back editor. `Y` at "Do you wish to
save?" calls `copy_message_list(&fight_messages[slot], scratch)`, which
frees the live slot's messages and puts fresh copies in, again reversed:
a plain edit restores the live order, and messages added with `N` end up
first. `save_messages_to_disk()` follows, `OLC_VAL` is cleared, and the
session goes on unless the save came from `Q`. The scratch copy keeps its
own strings; `cleanup_olc()` frees it with `free_message_list()`, which
frees the list struct too, and clears the `OLC_MSG` cursor. A slot that
started empty keeps a count one below its list length, so after a save
`skill_message()` ignores it (count 0) or never picks its last message,
until the next boot counts the records again.

**The file.** `save_messages_to_disk()` opens `MESS_FILE` with
`fopen(..., "w")`, with no temporary file, and if that fails it calls
`exit(1)`: the whole server stops without saving anyone. It writes a
header, then for each non-empty slot a `* <name> <type>` comment and one
`M` record per message (`#` for NULL). The close is not checked and
"Messages saved." is printed regardless; a record cut short by a failed
write makes `load_messages()` exit at the next boot. `doc/msgedit.txt` is
the legacy description of the editor.

**The quit flag.** `quit` in `msgedit_parse()` is a function-level `static`
shared by every session. `Q` with changes sets it, and whichever session
answers a save prompt next is ended and clears it: builder B's `S`, `Y` can
close B's editor while builder A's `Q`, `Y` leaves A in it. If A loses the
link at that prompt, the flag stays set.

**Link loss.** `close_socket()` calls `cleanup_olc(d, CLEANUP_ALL)`, which
frees the scratch list. Nothing is applied. `free_messages()` frees the
live messages at shutdown.

### ibtedit

`ibtedit` (in `ibt.c`, state `CON_IBTEDIT`) edits idea, bug and typo
reports with the same `d->olc` workspace (`OLC_IBT`). It is outside the
scope of this document; note only that `close_socket()` does not clean it
up on link loss.

## GenOLC: the generic library

The `gen*.c` files contain the operations the editors share: insert or
update an entry in an in-memory table, delete an entry, keep every stored
rnum consistent, and write a zone's file. They know nothing about
descriptors. `do_oasis_copy()`, `dig`, `buildwalk`, DG `oset` and the
`export` command use them too.

### Writing world files

All the zone writers follow the same protocol:

1. Validate the zone rnum; return `FALSE` if it is out of range.
2. Open `<prefix>/<zone vnum>.new` for writing.
3. Write every entry whose vnum lies in the zone's range, in vnum order,
   converting stored rnums back to vnums.
4. Write the terminator, then flush and close with `tb_fflush_checked()`
   and `tb_fclose_checked()`. If either fails, remove the `.new` file and
   return `FALSE`.
5. Replace `<zone vnum>.<ext>` with the `.new` file using
   `tb_replace_file_checked()`, so a failure never leaves a half-written
   world file in place.
6. Remove the zone's entry from the save list, and return success.

`save_rooms()`, `save_objects()`, `save_mobiles()` and `save_zone()` follow
all six steps. `save_rooms()`, `save_objects()` and `save_mobiles()` also
refuse to save the whole zone, removing the `.new` file, if one entry's
string section does not fit in `MAX_STRING_LENGTH`. A failed save leaves the
entry on the save list. Writers return `FALSE` (0) on failure;
`save_mobiles()` returns the number of bytes written on success.

### genolc.c

**The save list.** `save_list` is a singly linked list of
`struct save_list_data { zone, type }`, where `zone` is a zone **vnum** and
`type` is one of `SL_MOB`, `SL_OBJ`, `SL_SHP`, `SL_WLD`, `SL_ZON`, `SL_CFG`,
`SL_QST` (indexes into the `save_types[]` table of writers) or `SL_ACT`,
`SL_HLP` (socials and help, which cannot be saved automatically).
`add_to_save_list()` refuses `SL_CFG`, refuses a zone that does not exist
unless it is one of the `AEDIT_PERMISSION`/`HEDIT_PERMISSION` pseudo-zones,
and ignores duplicates. `remove_from_save_list()` unlinks and frees one
entry; `in_save_list()` tests for one.

**`save_all()`** walks the list from its head and calls each writer with
`real_zone(zone)`. It only moves past an entry when the writer returns a
negative number, and relies on a successful writer removing its own entry.
No writer ever returns a negative number, so:

- a writer that fails (read-only world directory, full disk, an oversized
  record) is called again for the same entry forever, which hangs
  `saveall` and shutdown;
- an entry with an unknown type is never passed either;
- `SL_ACT` and `SL_HLP` entries are skipped by moving the head without
  freeing the node, which also removes them from what `olc` reports even
  though nothing was saved.

**String helpers.** `str_udup()` duplicates a string, substituting
"undefined" for NULL or empty input. `str_udupnl()` does the same and adds
"\r\n". `strip_cr()` removes carriage returns in place.
`genolc_checkstring()` runs `smash_tilde()` and `parse_at()` on builder
input and always returns `TRUE`. `sprintascii()` turns a 32-bit flag word
into the letter form used in world files (`a`-`z`, `A`-`F`, or `0` when no
bit is set).

**Extra descriptions.** `copy_ex_descriptions(&to, from)` always allocates
at least one node, so callers check `from` first (both `copy_room_strings()`
and `copy_object_strings()` do). `free_ex_descriptions()` frees a whole
list and logs if handed NULL.

**Zone export.** `do_export_zone()` (`export <zone>`, `LVL_IMPL`, not on
Windows) writes the zone in a portable form to `lib/world/export/`: every
vnum becomes `QQnn` (the last two digits), exits that leave the zone become
`ZZnn`, and a `qq.info` file describes the result. It then runs `tar` and
`gzip` with `fork()` and `execvp()` (no shell) and reports their exit
status. The per-file writers return `TRUE` without checking that their
data reached the disk, so on a full disk the archive is built from
truncated files and the export is reported as a success. The info file is
written first but reads a file-scope
counter (`zone_exits`) that only the later room export fills, so it
describes the previous export's exits; in practice it almost always says the
zone has none.

### genwld.c (rooms)

**`add_room(room)`** is both update and insert.

- *Update* (the vnum exists): extract the live room's script, save the
  `people` and `contents` heads, `copy_room()` the scratch room over
  `world[i]`, restore the two heads, add the zone to the save list.
- *Insert*: grow `world[]` by one with `RECREATE()` (which may move the
  whole array), walk down from the top shifting each room up one slot until
  the insertion point, then struct-copy the new room in and duplicate its
  strings. For each shifted room it calls `update_wait_events()` and adds
  one to `IN_ROOM()` of every character and object in it. Afterwards it
  shifts room rnums at or above the new one in reset commands (`'M'`, `'O'`,
  `'T'`, `'V'` argument 3; `'D'`, `'R'` argument 1), in the three start-room
  rnums and in every exit of every room. It returns the new rnum.

The reset-command scan in the insert path starts at the new room's own zone,
so a command in a lower zone that refers to a room above the insertion point
keeps a stale rnum. `GET_WAS_IN()` (the room a linkless player returns to)
is not renumbered by either function.

**`delete_room(rnum)`** refuses rnum 0 (the Void) and out-of-range values.
It moves the room's objects and characters to the Void, frees the room's
strings, extracts its script, frees its trigger list and cancels its mud
events. Exits that led to the room are freed if they had no keyword or
description, or pointed at `NOWHERE` if they did; exits above the room are
shifted down. Reset commands that refer to the room are turned into `'*'`
(cancelled) rather than removed, and the others are shifted down. Shop room
entries equal to the room's **vnum** are set to 0, so the shop lists the
Void instead of dropping the entry. Then the rooms above are shifted down (with
`update_wait_events()` and `IN_ROOM()` fix-ups) and `world[]` shrinks. A
start room equal to the deleted rnum is reset to the Void, but start rooms
**above** it are not shifted down, so after a delete new and recalling
players arrive in the room after the start room, and a start room that was
the last room points past the end of `world[]`.

**`save_rooms(zone)`** writes `<zone>.wld`. A door is written as 1, or 2 if
pickproof, plus 2 if hidden; the hidden flag of a non-door exit is lost.
The extra-description loop passes `xdesc->description` to `strncpy()`
without a NULL check.

**`copy_room(to, from)`** frees `to`'s strings, struct-copies `from`, then
duplicates `from`'s strings into `to`, moves `from`'s event list to `to`
and clears `from`'s `people`, `contents` and `events`.
`copy_room_strings(dest, source)` duplicates name, description, exits and
extra descriptions without freeing what `dest` had, so it is only correct
right after a struct copy. `free_room_strings()` frees them and NULLs the
exit pointers.

### genobj.c (object prototypes)

**`add_object(obj, vnum)`**:

- *Update*: `copy_object(&obj_proto[rnum], obj)` (frees the prototype's
  strings, struct-copies the scratch object, duplicates its strings), then
  `update_all_objects()` rewrites every live instance (see
  [What a save does to live instances](#what-a-save-does-to-live-instances)).
- *Insert*: `insert_object()` grows `obj_index[]` and `obj_proto[]` with
  `RECREATE()`, shifts entries up (fixing each shifted prototype's
  `item_number`) and calls `index_object()`, which sets the vnum, a zero
  instance count and no special procedure and copies the object in with
  `copy_object_preserve()` (no freeing, because the slot's old pointers now
  belong to the shifted neighbour). `adjust_objects(rnum)` then shifts
  object rnums at or above the new one in live objects (skipping
  `NOTHING`), in reset commands (`'P'` arguments 1 and 3; `'O'`, `'G'`,
  `'E'` argument 1; `'R'` argument 2), in the bulletin board table
  (`BOARD_RNUM`) and in shop product lists. The board shift has no
  `NOTHING` guard: a board whose object does not exist has rnum
  `NOTHING`, and 65535 + 1 wraps to rnum 0.

`copy_object_main()` ignores its `free_object` argument; `copy_object()`
frees `to`'s strings first and `copy_object_preserve()` does not. Neither
frees `to`'s trigger list.

**`delete_object(rnum)`**:

1. For every live instance, move its contents out and `extract_obj()` it.
   The rescue test is `if (IN_ROOM(tmp))`, which is true for `NOWHERE`
   and false for room 0: contents of a carried, worn or nested container
   are handed to `obj_to_room(..., NOWHERE)`, which refuses them and leaves
   them in `object_list` with no location at all; contents of a container
   lying in the Void are not rescued and are extracted with it. The scan
   keeps a `next` pointer across `extract_obj()`.
2. `assert()` that no instance remains.
3. Shift live object rnums greater than `rnum` down by one. There is no
   `NOTHING` guard, so every object without a prototype (corpses, money)
   becomes rnum 65534, and extracting it later reads and writes
   `obj_index[65534]` and `obj_proto[65534]`, far past the arrays.
4. Shift `obj_index[]` and `obj_proto[]` down over the deleted slot,
   without freeing the deleted prototype's strings, extra descriptions or
   trigger list, and shrink both arrays.
5. Shift bulletin board rnums and shop products greater than `rnum`. A
   board or product **equal** to `rnum` keeps its value and so now names
   the next prototype.
6. Delete reset commands that load the object (`delete_zone_command()`),
   otherwise shift them. After a deletion the loop still advances, so the
   command that slid into the slot is skipped, a `'P'` deleted on argument
   3 falls through and examines the next command's argument 1, and deleting
   the last command before `'S'` reads past the shrunken list. The zones
   whose commands changed are not added to the save list.
7. Save the object's zone file immediately.

**`save_objects(zone)`** writes `<zone>.obj` and skips (with a log line)
any extra description whose keyword or description is NULL or empty.

**Instance string helpers.** Live instances share their prototype's
strings unless restrung. `free_object_strings()` frees all of an object's
strings; `free_object_strings_proto()` frees only those that differ from
the prototype's. The `oset_*` setters used by `do_oset()` and DG `oset`
replace one string or apply, freeing the old string only when it is not the
prototype's.

### genmob.c (mobile prototypes)

**`add_mobile(mob, vnum)`**:

- *Update*: `copy_mobile(&mob_proto[rnum], mob)` frees the prototype's
  strings, struct-copies the scratch mobile, fixes empty strings with
  `check_mobile_strings()` and duplicates the strings. Then
  `update_mobile_strings()` points each live instance's name, title, short,
  long and detailed descriptions at the new prototype strings. Nothing else
  about a live mobile changes: statistics, flags, level and so on stay as
  they were until the mobile is reloaded. A field that is NULL in the new
  prototype is skipped, so an instance keeps a pointer to the old
  prototype string that `copy_mobile()` just freed. Restrung instance
  strings are overwritten without being freed.
- *Insert*: grow `mob_proto[]` and `mob_index[]` with `RECREATE()`, shift
  entries up (incrementing each shifted prototype's `nr`), copy the new
  prototype in with duplicated strings, zero instance count and no special
  procedure. Then shift mobile rnums at or above the new one in live
  characters (skipping `NOBODY`, which players have), in `'M'` reset
  commands and in shop keepers (skipping `NOBODY`).

**`delete_mobile(rnum)`**:

1. `extract_mobile_all()` frees every live instance's non-prototype
   strings, sets the string pointers to NULL and calls `extract_char()`,
   which only marks the mobile `MOB_NOTDEADYET`; it stays in its room, with
   NULL names, until `extract_pending_chars()` runs at the end of the pulse.
2. `extract_char()` is also called on the **prototype**, which is not in
   `character_list`. That increments `extractions_pending` for an
   extraction that can never happen, so from then on every pulse walks the
   whole character list and logs "Couldn't find 1 extractions as counted".
3. Shift `mob_index[]` and `mob_proto[]` down (without freeing the deleted
   prototype's strings or trigger list) and shrink them.
4. Shift every live character's `nr` that is `>= rnum` down by one, with no
   `NOBODY` guard: every player's `nr` becomes 65534, and the doomed
   instances (whose `nr` equalled `rnum`) now name the previous prototype,
   whose instance count `extract_char_final()` then decrements. That count
   is what zone resets compare with the maximum, so the previous mobile can
   be over-loaded afterwards.
5. Delete `'M'` reset commands for the mobile and shift the others, with the
   same skip-after-delete loop as `delete_object()`. Dependent `'G'` and
   `'E'` commands that followed a deleted `'M'` are left in place.
6. Shift every shop keeper `>= rnum` down, again with no `NOBODY` guard, so
   a keeperless shop gets keeper 65534 and a shop whose keeper was the
   deleted mobile is silently handed to the previous one.
7. Save the mobile's zone file immediately.

**`free_mobile(mob)`** frees a scratch mobile: all strings and the trigger
list if `nr` is `NOBODY`, otherwise only the strings and trigger list that
differ from the prototype's; then affects, script and the struct.
`check_mobile_strings()` replaces an empty name or description with "An
undefined string." It reads `mob_index[mob->nr]` to name the mobile in its
log line, which would be out of bounds for a mobile whose `nr` is `NOBODY`
(a new mobile's scratch copy). Its callers today pass prototypes or the
scratch copy of an existing mobile.

**`save_mobiles(zone)`** writes `<zone>.mob` with `write_mobile_record()`
and `write_mobile_espec()` (the `E` block of non-default abilities and
saves).

### genzon.c (zones)

**`real_zone_by_thing(vnum)`** binary-searches `zone_table[]` for the zone
whose `[bot, top]` range contains a room, object, mobile or shop vnum. It
assumes the ranges are sorted and disjoint; nothing enforces that.

**`create_new_zone(vnum, bottom, top, &error)`** is called by `zedit new`.
It rejects a zone vnum above 655, `bottom > top`, `bottom == 0` and a top
at `IDXTYPE_MAX`, and rejects a duplicate zone vnum, but its duplicate loop
stops at `i < top_of_zone_table` and never compares the **last** zone.
It does not check that the new room range is disjoint from existing zones.
It then creates the seven world files (`.zon`, `.wld`, `.mob`, `.obj`,
`.shp`, `.qst`, `.trg`) with plain `fopen(..., "w")`, so creating a zone
with the last zone's number truncates that zone's files on disk, and a
second zone with the same number is appended in memory. A failure part way
leaves the files already created. `create_world_index()` adds the zone to
each `index` file through a `newindex` temporary file and an atomic
replace, and leaves the index alone if it already lists the zone.
`zone_table[]` grows with `RECREATE()`; zones above the new one are shifted
up and every room of a shifted zone gets `world[].zone++`. The new zone has
the name "New Zone", builders "None", lifespan 30, reset mode 2, no flags,
levels -1 and an empty command list, and is put on the save list.

**`save_zone(zone)`** writes `<zone>.zon`, converting each command's rnums
back to vnums by indexing `mob_index[]`, `obj_index[]`, `world[]` and
`trig_index[]` without range checks, skipping `'*'` commands and writing a
`(comment)` with the target's name. Zone flags and level limits are written
only when set.

**Reset-command lists.** A zone's commands are an array terminated by a
command `'S'`. `count_commands()` counts them. `add_cmd_to_list(&list,
cmd, pos)` builds a new array one longer with `cmd` struct-copied in at
`pos` and frees the old array; a `pos` past the end is silently ignored.
`new_command(zone, pos)` validates `pos` and inserts a blank command `'N'`;
only its `command` field is set, so `if_flag`, the three arguments, `line`
and the `sarg` pointers are uninitialised stack bytes until the editor
fills them in.
`delete_zone_command(zone, pos)` validates `pos` and removes the command.
`remove_room_zone_commands(zone, room)` removes every command that belongs
to a room. A command belongs to the room named by its argument 3 (`'M'`,
`'O'`, `'T'`, `'V'`) or argument 1 (`'D'`, `'R'`); `'G'`, `'E'` and `'P'`
commands have no room of their own and inherit the room of the command
before them, which is exactly the "equip the mobile just loaded" meaning
they have in a zone reset. All of these copy commands by struct, so the
`sarg1` and `sarg2` strings of `'V'` commands are shared, never duplicated
or freed by the list helpers.

### genshp.c and genqst.c

**Storage.** In `struct shop_data` (`shop.h`), `keeper` is a mob **rnum**
(`NOBODY` for none) and `producing` holds object **rnums** despite its
`obj_vnum *` type (`shop.c` converts both from vnums at boot); `in_room`
holds room **vnums**. Both lists end in `NOTHING` (equal to `NOWHERE`).
The `type` list ends with an entry of type `NOTHING`, and an entry's
`keywords` may be NULL. `func`, the keeper's own special procedure, is not
saved. `struct aq_data` (`quest.h`) holds only vnums. No shop or quest
rnum is stored anywhere, so neither table's inserts or deletes renumber
anything.

**Copies.** `copy_shop(to, from, free_old_strings)` deep-copies the lists
and messages (`str_udup()` turns a NULL message into "undefined"). It
frees `to`'s lists whenever their pointers are non-NULL, whatever
`free_old_strings` says, and reads `from`'s lists unchecked. `free_shop()`
frees all of it; the list helpers build a new array and free the old one.
`copy_quest(to, from, free_old_strings)` copies every field and duplicates
the strings; `quest.h` declares it, and `copy_quest_strings()`, with the
parameter names reversed.

**Insert and update.** `add_shop()` and `add_quest()` copy and never take
ownership. An update copies over the live entry, freeing its old strings;
an insert grows the table with `RECREATE()`, shifts later entries up by
struct copy and fills the slot without freeing (`add_shop()` first NULLs
the slot's list pointers, which belong to the shifted neighbour). Both
queue the zone for saving, and `add_quest()` makes `questmaster` the
questmaster's special procedure, keeping any other in `QST_FUNC`.

**`delete_quest(rnum)`** reads `QST_MASTER(rnum)` before it checks
`rnum`, so `NOTHING` reads far past the table. It frees the strings,
shifts the rest down, shrinks the table and queues the zone. When the
questmaster has no quest left it runs `mob_index[qm].func = tempfunc`,
but `qm` is a mob **vnum**: another mobile's procedure is replaced, or the
write lands past the end of `mob_index[]`, and the questmaster keeps
`questmaster()`.

**Writers.** `save_shops()` and `save_quests()` follow
[Writing world files](#writing-world-files), but `save_shops()` leaves its
`.new` file when the header write fails, `save_quests()` logs and drops a
record longer than `MAX_STRING_LENGTH` yet commits the file, and neither
learns whether `create_world_index()` worked (it returns `void`).
`save_shops()` converts product and keeper rnums through `obj_index[]` and
`mob_index[]` unchecked, so a stale rnum reads out of bounds, and writes
an accepted item type as its number directly followed by the namelist,
which boot misreads when the namelist begins with a digit.

## What a save does to live instances

A prototype is a template; live instances were created from it earlier.
Each editor's save propagates changes to existing instances differently.

| Editor | Live instances after a save |
| --- | --- |
| `redit` | There is exactly one live room, updated in place (`add_room()` / `copy_room()`). Occupants and contents stay; the script is rebuilt; the event list is dropped; `light`, `func` and other plain fields come from the snapshot taken when the editor opened. |
| `oedit` | Every instance is overwritten with the prototype by `genobj.c:update_all_objects()`: `*obj = *proto`, then only the UID, location fields (`in_room`, `carried_by`, `worn_by`, `worn_on`, `in_obj`), the `contains`, `next_content` and `next` links and `sitting_here` are put back. Strings become the prototype's (restrung or iedited text is lost and leaked). Type, values, weight, applies, affect bits, timer and cost are replaced even for worn, carried or contained objects, so the same accounting drift as `iedit` follows; containers also lose the weight of their contents. The instance's running script and private trigger list are overwritten without being freed, then each instance gets new triggers. Its `events` pointer becomes the prototype's (objects never hold mud events in this code base, so this is latent). |
| `medit` | Strings and triggers change; nothing else does. `add_mobile()` points each instance's name, title and three descriptions at the new prototype strings (`update_mobile_strings()`; restrung text is leaked). `medit_save_internally()` then extracts each instance's running script (its variables and pending `wait`s are lost), replaces its private trigger list with a copy of the new one and calls `assign_triggers()`. Level, statistics, flags, affect bits, position, gold and experience are not touched; each mobile keeps its own until it is reloaded. Mobiles loaded later copy the whole new prototype, including its `nr` (see [medit](#medit-mobile-prototypes) for when that is wrong). |
| `iedit` | Edits one instance in place; see [iedit](#iedit-one-live-object). |
| `sedit`, `qedit`, `trigedit` | Shops and quests have no instances. `sedit`: `add_shop()` overwrites the `shop_index[]` entry, and the keeper uses it from the next command typed in its room, because `shop.c:shop_keeper()` looks its shop up by keeper rnum on every call; the shop's banked gold returns to its value when the editor opened, and the keeper's special procedure was replaced when the keeper was chosen, not at save. `qedit`: `add_quest()` overwrites the `aquest_table[]` entry and installs `questmaster()` on the questmaster; players already on the quest keep the time limit and goal count copied when they joined but read type, target and rewards by vnum, so they see the rest of the change at once. `trigedit` rebuilds the prototype command list that live triggers share; see [dg-scripts.md](dg-scripts.md#trigger-olc). |

## Renumbering on insert and delete

The world, object, mobile, zone, shop, quest and trigger tables are arrays
sorted by vnum. Inserting an entry shifts every later entry up by one;
deleting shifts them down. Any rnum stored anywhere must be adjusted at the
same time, or it silently names the neighbouring entry. The arrays are also
grown and shrunk with `RECREATE()` (`realloc()`), which may move them, so
any **pointer** into one of them becomes invalid.

### Who adjusts which stored rnums

| Table | Stored rnums | Adjusted on insert by | Adjusted on delete by |
| --- | --- | --- | --- |
| `world[]` | `IN_ROOM()` of characters and objects in rooms | `add_room()` (only rooms it shifts, which covers everyone in a room) | `delete_room()` |
| | exits (`to_room`) | `add_room()`, every room | `delete_room()`, every room |
| | reset commands | `add_room()`, **from the new room's zone up** | `delete_room()`, all zones (matching commands become `'*'`) |
| | `r_mortal_start_room`, `r_immort_start_room`, `r_frozen_start_room` | `add_room()` | `delete_room()` only if equal; **not shifted** above |
| | `GET_WAS_IN()` of linkless players | **nobody** | **nobody** |
| | room DG wait events (pointer, not rnum) | `update_wait_events()` for shifted rooms only | same |
| | room mud events (`pStruct` pointer) | **nobody** | **nobody** (the deleted room's own events are cancelled) |
| | other `redit` sessions' exits, `zedit` sessions' commands | `redit_save_internally()`, only when the scratch `number` was `NOWHERE` | **nobody** |
| `obj_proto[]` | live objects' `item_number` | `adjust_objects()` | `delete_object()`, **no `NOTHING` guard** |
| | reset commands | `adjust_objects()` | `delete_object()` (deletes matches, skip-after-delete bug) |
| | `BOARD_RNUM()` | `adjust_objects()` | `delete_object()` (equal value not handled) |
| | shop products | `adjust_objects()` | `delete_object()` (equal value not handled) |
| | other `sedit` products, `zedit` commands | `oedit_save_internally()` on a new object | **nobody** |
| `mob_proto[]` | live mobiles' `nr` | `add_mobile()` | `delete_mobile()`, **no `NOBODY` guard** |
| | `'M'` reset commands | `add_mobile()` | `delete_mobile()` (deletes matches, skip-after-delete bug) |
| | shop keepers | `add_mobile()` | `delete_mobile()`, **no `NOBODY` guard**, equal value handed to the previous mobile |
| | other sessions | `medit_save_internally()`, for a new vnum only (judged by `real_mobile(OLC_NUM(d))`, so `W` and `mcopy` count): `CON_MEDIT` scratch `nr` (the saving session's own included) and `CON_SEDIT` keepers, skipping `NOBODY`, and argument 1 of `'M'` commands in `CON_ZEDIT` sessions, with no sentinel guard | **nobody**: scratch `nr`, keepers and `'M'` commands keep the old rnum, and the next save of a stale `medit` session copies its wrong `nr` into the prototype |
| `zone_table[]` | `world[].zone` | `create_new_zone()` | (zones are never deleted) |
| | other sessions' `OLC_ZNUM`, scratch rooms' `zone` | `zedit.c:zedit_new_zone()` | |
| | the zone reset queue (`reset_q`) | **nobody** | |
| `trig_index[]` | live triggers' `nr`, other `trigedit` drafts | `trigedit_save()` (see [dg-scripts.md](dg-scripts.md#trigger-olc)) | (triggers are never deleted) |
| | `'T'` reset commands, live and in `zedit` sessions | **nobody** | |
| `shop_index[]`, `aquest_table[]` | none: no shop or quest rnum is stored; `shop.c:shop_keeper()` finds its shop by keeper rnum on every call, and players' current and completed quests and the quest links hold vnums resolved with `real_quest()` | nothing to adjust (`add_shop()`, `add_quest()`) | shops cannot be deleted; `delete_quest()` has nothing to adjust, but players on the quest and quests linking to it keep its vnum (see [qedit](#qedit-quests)) |

### Pointer stability

`RECREATE()` of `world[]` in `add_room()` and `delete_room()` invalidates
every pointer into the old array. Two kinds of such pointers exist:

- **DG wait events of room triggers** keep the room's address as their
  owner. `update_wait_events()` repoints the triggers of each room the loop
  shifts, but rooms below the insertion point keep the old address. When
  such a wait expires, `dg_scripts.c:trig_wait_event()` scans `world[]` for
  the address, does not find it and drops the trigger with "Trigger
  restarted on unknown entity" (or, if a later reallocation put `world[]`
  back at the old address, resumes it on whatever room is there). See
  [dg-scripts.md](dg-scripts.md#the-event-queue).
- **Room mud events** (the darkness spell, `magic.c`) keep a raw
  `struct room_data *` in `mud_event_data.pStruct`. Neither function
  repoints them, so an active darkness event reads freed or shifted memory
  when it expires. See [gameplay.md](gameplay.md#events-and-generic-lists).

`obj_proto[]`, `mob_proto[]` and `zone_table[]` move the same way. No code
keeps a pointer into them across an OLC save, but new code must not either:
store a vnum, or an rnum that the table's owner renumbers.

### Rules for code that shifts rnums

- Shift with an explicit sentinel test:
  `if (x != NOWHERE && x >= n) x++;` and `if (x != NOWHERE && x > n) x--;`.
  With unsigned 16-bit indexes the sentinel is the largest value and
  passes every `>=` test.
- Decide what an rnum **equal** to the deleted entry should become (cancel
  the reference, drop it, or point it at a safe default) and report it.
  Leaving it unchanged makes it name the next entry.
- When deleting entries from an array while iterating over it, do not
  advance the index after a deletion.
- Renumber every place that stores the rnum: live data, scratch copies in
  other sessions, and caches such as start rooms and board rnums.

### Invariants

- After any OLC save or delete, every stored rnum must again name the same
  vnum it named before. The table above lists where HEAD fails this.
- Vnums are always safe to store across a save. Prefer them in new data
  structures (shops already store their rooms as vnums).
- Never keep a pointer into `world[]`, `obj_proto[]`, `mob_proto[]` or
  `zone_table[]` across anything that can insert or delete.

## Builder permissions

### Who can run which command

Command levels come from `interpreter.c:cmd_info[]`.

| Level | Commands |
| --- | --- |
| everyone (0) | `prefedit`, `toggle` |
| `LVL_BUILDER` | `redit`, `oedit`, `iedit`, `medit`, `zedit`, `sedit`, `qedit`, `trigedit`, `rlist`, `olist`, `mlist`, `tlist`, `slist`, `qlist`, `zlist`, `dig`, `buildwalk`, `olc`, `saveall`, `vdelete`, `zreset`, `zcheck`, `zpurge` |
| `LVL_GOD` | `rcopy`, `ocopy`, `mcopy`, `scopy`, `tcopy`, `aedit`, `hedit`, `tedit`, `msgedit`, `zlock`, `zunlock`, `links` |
| `LVL_IMPL` | `cedit`, `export`; inside `zedit`, creating zones (`zedit new`) |

### The builder's OLC zone

Each player has a saved field, `olc_zone`, read with `GET_OLC_ZONE(ch)`. It
holds a zone **vnum**, `NOWHERE` (no assignment), or one of three sentinels
from `oasis.h`:

| Value | Meaning |
| --- | --- |
| `ALL_PERMISSION` (666) | Every zone. |
| `HEDIT_PERMISSION` (888) | The help editor. |
| `AEDIT_PERMISSION` (999) | The social editor. |

`set <player> olc <zone | all | hedit | aedit | off>` (`LVL_GRGOD`,
`act.wizard.c:perform_set()`) assigns it. The sentinels cannot collide with
real zone vnums because `create_new_zone()` caps zone vnums at 655.
`hedit` and `aedit` pass `HEDIT_PERMISSION` and `AEDIT_PERMISSION` to
`can_edit_zone()` as pseudo zone rnums.

### can_edit_zone()

`oasis.c:can_edit_zone(ch, rnum)` decides, in this order:

1. No descriptor, an NPC, or `rnum == NOWHERE`: **refuse**.
2. The zone is flagged `ZONE_NOBUILD` (and `rnum` is not one of the
   pseudo-zones): **refuse**, whatever the builder's level. `zunlock` must
   clear the flag first.
3. `GET_OLC_ZONE(ch) == ALL_PERMISSION`: **allow**.
4. The builder holds the matching `HEDIT_PERMISSION` or `AEDIT_PERMISSION`
   for that pseudo-zone: **allow**.
5. `GET_LEVEL(ch) >= LVL_GRGOD`: **allow**.
6. The builder's name is in the zone's `builders` string (`is_name()`;
   not checked for the pseudo-zones): **allow**, with no level check.
7. `GET_OLC_ZONE(ch) == NOWHERE`: **refuse**.
8. `GET_LEVEL(ch) < LVL_BUILDER`: **refuse**.
9. `real_zone(GET_OLC_ZONE(ch)) == rnum`: **allow**; otherwise **refuse**.

`send_cannot_edit()` tells the builder which zone they may edit and logs the
attempt at `LVL_IMPL`.

The zone's `builders` string is edited in `zedit`. `zlock` and `zunlock`
(`act.wizard.c:do_zlock()`, `do_zunlock()`) set and clear `ZONE_NOBUILD`;
for one zone they allow `LVL_GRGOD`, anyone in the zone's builders list,
and a builder whose OLC zone is that zone. `zlock all` (every zone with the
`ZONE_GRID` flag) and `zlock all all` (every zone) need `LVL_GRGOD`;
`zlock list` shows the locked zones. The flag is written to the zone file
immediately with `save_zone()`.

### Where permission is checked

- Every world editor checks `can_edit_zone()` once, at entry, for the zone
  of the vnum being edited. `delete` and `save` inside the session do not
  re-check.
- `do_oasis_copy()` checks only the **target** zone; any zone can be the
  source.
- `do_dig()` checks both the builder's current zone and the target zone.
- `buildwalk()` checks the builder's current zone.
- `iedit` checks nothing beyond the command level.
- `can_edit_zone()` is only as good as its `rnum` argument: it indexes
  `zone_table[rnum]` for the `ZONE_NOBUILD` test without a range check.

## Listing, copying and digging

### oasis_list.c

`do_oasis_list()` implements `rlist`, `olist`, `mlist`, `tlist`, `slist`,
`qlist` and `zlist`. With no argument (or `.`) it lists the zone the builder
stands in; with one number, that zone; with two numbers, a vnum range.
`zlist <name>` lists zones by builder. `olist` also has `type`, `affect` and
name searches, and `mlist` has `level` and `flags` searches. Output is
formatted into fixed `MAX_STRING_LENGTH` buffers and paged.

`mlist level` and `mlist flags` (`perform_mob_level_list()`,
`perform_mob_flag_list()`) load every matching mobile with `read_mobile()`,
put it in the Void, read its name and level and `extract_char()` it. The
mobiles are real instances, with scripts, until the end of the pulse.
`do_oasis_links()` is compiled but not registered as a command; `links`
runs `act.wizard.c:do_links()`.

### oasis_copy.c

**`do_oasis_copy()`** implements `rcopy`, `ocopy`, `mcopy`, `scopy` and
`tcopy` (`LVL_GOD`). The command's `subcmd` is the editor's connection
state, which selects a row of a table holding the lookup, setup and save
functions. It requires that the source exists and the target does not, that
no descriptor in that editor state is editing the target vnum, and that the
builder may edit the target's zone. It then creates a workspace, sets
`OLC_NUM(d)` to the target vnum, calls the editor's `setup_existing()` on
the source and its `save_internally()`, and ends with
`cleanup_olc(d, CLEANUP_ALL)`. The descriptor never leaves `CON_PLAYING`, so
other sessions' renumbering loops do not see it. Both vnums are parsed with
`tb_parse_int_or_zero()` and narrowed to 16 bits by the lookups, so a vnum
above 65535 wraps. For `rcopy` the scratch room carries the source vnum,
so `redit_save_internally()` treats the insert as an update and other
sessions are not renumbered.

**`do_dig(dir, vnum [name])`** (`LVL_BUILDER`) creates an exit from the
builder's room, and the reverse exit if the target has none in the opposite
direction. `dig <dir> -1` removes an exit. Digging to vnum 0 is refused. If
the target room does not exist, `do_dig()` creates it by filling a scratch
room in a fresh workspace and calling `redit_save_internally()`, then
`cleanup_olc(d, CLEANUP_ALL)`. New exits have key 0 rather than `NOTHING`.
The target vnum is narrowed to 16 bits, so `dig north 65535` equals
`NOWHERE` and removes the exit.

**`buildwalk(ch, dir)`** is called by `act.movement.c:perform_move()` when
the exit does not exist. If the builder has `PRF_BUILDWALK`, is at least
`LVL_BUILDER` and may edit the current zone, it finds the first unused room
vnum in the zone (`redit_find_new_vnum()`), creates the room through
`redit_save_internally()`, links both exits and lets the move proceed. It
releases the workspace with `CLEANUP_STRUCTS`, which frees the scratch room
struct but leaks its name and description (the world copy has its own).
Neither `buildwalk()` nor `do_dig()` checks `ch->desc` before using it;
both rely on `can_edit_zone()`, which refuses a character without a
descriptor, running first.

### oasis_delete.c

`free_strings(data, type)` frees the strings of a room (`OASIS_WLD`, which
then calls itself with `OASIS_EXI` to free the exits) or of a configuration
copy (`OASIS_CFG`: the four play messages, default IP and directory, log
name, menu, welcome and start messages). `OASIS_MOB` and `OASIS_OBJ` return
`FALSE` and free nothing. It is used by `cleanup_olc()`'s `free_config()`
and when the server frees `config_info` at shutdown.

## Invariants checklist

Use this list when adding an editor or changing one. Where HEAD breaks a
rule, the section that describes the code says so.

**Sessions**

1. `d->olc` is created by an entry command (or by `do_oasis_copy()`,
   `do_dig()` or `buildwalk()`) and released only by `cleanup_olc()`. Never
   `free(d->olc)` once scratch data hangs off it.
2. Open a session only from `CON_PLAYING` and only when `d->olc` is NULL.
3. A new editor state needs an entry in `nanny()`'s `olc_functions[]`
   (unless, like `tedit`, it lives entirely in the string editor), in
   `close_socket()`'s cleanup switch, and, if it uses the string editor, in
   both `string_add()` tables (abort and cleanup callback).
4. Check permission at entry with `can_edit_zone()` on the zone of the vnum
   being edited, and check it again before any destructive action whose
   target can differ from that vnum.

**Ownership**

5. A scratch copy owns deep copies of everything it can free. GenOLC
   duplicates strings again when it installs a copy, so the scratch copy is
   always freed afterwards by `cleanup_olc()`.
6. `OLC_SCRIPT` is the one structure handed over by pointer: after a save
   the prototype owns it. Every path that does not save must free it.
7. `OLC_DESC` and `OLC_MSG` are cursors into lists owned by the scratch
   copy.
8. While the string editor is active, `d->str` points into the scratch
   copy; do not free, replace or reallocate the scratch copy until the
   editor finishes.
9. Live instances share their prototype's strings unless restrung. Free an
   instance string only if it differs from the prototype's
   (`free_object_strings_proto()`, `free_mobile()`).
10. Reset-command lists are copied by struct; the `sarg1`/`sarg2` strings of
    `'V'` commands are shared between the live zone and a `zedit` copy.

**Lifetime**

11. Everything a scratch copy took by struct assignment is a snapshot of the
    moment the editor opened. Writing it back overwrites any change made in
    the meantime (`redit` and the room `light` counter, for example).
12. Between two input lines, live objects, mobiles and rooms can be
    extracted, moved or renumbered. Keep vnums or UIDs, not pointers or
    rnums, unless something renumbers what you keep. `iedit` revalidates its
    object with `obj_uid_matches()` and never dereferences the saved pointer
    to do so.
13. Changing an object's weight, applies, affect bits or type while it is
    worn, carried or contained must go through the handler (take it out,
    change it, put it back), or the owner's accounting drifts.

**Real numbers**

14. `NOWHERE`, `NOTHING` and `NOBODY` are 65535 and compare greater than
    every rnum. Exclude them explicitly in every shift.
15. Every insert or delete must adjust live data, reset commands, start
    rooms, boards, shops, quests, and the scratch copies of every other open
    session, and must decide what happens to a reference equal to a deleted
    entry.
16. Do not keep pointers into `world[]`, `obj_proto[]`, `mob_proto[]` or
    `zone_table[]` across an insert or delete.
17. Zone room ranges must stay sorted and disjoint; `real_zone_by_thing()`
    depends on it.
18. Loops that delete entries from an array they are walking must not
    advance after a deletion.

**Files**

19. Write `<zone>.new`, check flush and close, then replace the real file
    with `tb_replace_file_checked()`. Remove the save-list entry only on
    success, and return `FALSE` on failure.
20. Anything that loops over the save list must make progress when a writer
    fails (`save_all()` does not).

**Editors**

21. A scratch mobile's `nr` must be `real_mobile(OLC_NUM(d))`, or `NOBODY`
    for a vnum not yet saved, whenever it is saved, freed or deleted:
    `add_mobile()` copies it into the prototype and `read_mobile()` copies
    the prototype's into every instance. Reset it after `W`, and renumber it
    on every insert and delete of a mobile.
22. Shop lists are never NULL and always end in their sentinel. `keeper` and
    `producing` hold rnums and must follow `mob_proto[]` and `obj_proto[]`
    through every insert and delete; `in_room` and every quest field hold
    vnums. Resolve `QST_MASTER()` with `real_mobile()` before indexing
    `mob_index[]`, and range-check every rnum a writer turns back into a
    vnum.
23. `add_shop()` and `add_quest()` copy; they never take ownership. The
    editor frees its whole scratch structure afterwards (`free_shop()`,
    `free_quest()`), not just the struct. A new GenOLC add function must
    say which it does, and the editor's cleanup type must match.
24. Change a mobile's `mob_index[].func` only when an edit is saved. Keep
    the procedure it replaces in exactly one place (`S_FUNC` or
    `QST_FUNC`), give it back when the mobile stops being a keeper or
    questmaster, and never store `shop_keeper` or `questmaster` as the
    other's secondary procedure: the two would call each other until the
    stack overflows.
25. A reset command's room is implied by its position: `'M'`, `'O'`, `'T'`
    and `'V'` name it in argument 3, `'D'` and `'R'` in argument 1, and
    `'G'`, `'E'`, `'P'` and `'*'` belong to the command before them.
    `zedit_setup()`, `genzon.c:remove_room_zone_commands()` and
    `act.wizard.c:list_zone_commands_room()` must apply the same rule, and
    code that inserts, removes or reorders commands must keep every `'G'`,
    `'E'` and `'P'` directly behind the `'M'` or `'O'` it serves.
26. A save may refuse only what `db.c:reset_zone()` would refuse. A
    save-time check stricter than the interpreter deletes valid commands from
    the world files the next time anyone saves the room, as the
    `'G'`/`'E'`/`'P'` rule of `zedit_save_internally()` does.
27. Reset commands, in `zone_table[]` and in every `CON_ZEDIT` scratch list,
    hold rnums of four tables: `mob_proto[]` (`'M'` argument 1),
    `obj_proto[]` (argument 1 of `'O'`, `'G'`, `'E'` and `'P'`, argument 3 of
    `'P'`, argument 2 of `'R'`), `world[]` (argument 3 of `'M'`, `'O'`, `'T'`
    and `'V'`, argument 1 of `'D'` and `'R'`) and `trig_index[]` (argument 2
    of `'T'`). An insert into or delete from any of them must renumber both
    places; `trigedit_save()` renumbers neither.
28. Zone rnums live in `world[].zone`, in `OLC_ZNUM(d)` of `CON_REDIT`,
    `CON_ZEDIT`, `CON_MEDIT`, `CON_SEDIT`, `CON_OEDIT`, `CON_TRIGEDIT` and
    `CON_QEDIT` sessions, and in the reset queue `reset_q`; creating a zone
    must shift all three (the queue is missed today). `zone_table[].name` and
    `builders` are never NULL: the loader and `create_new_zone()` always set
    them, and `zedit` copies them with unchecked `strdup()`.
29. `OLC_ZNUM` in `aedit` and `hedit` and `OLC_NUM` in `msgedit` are raw
    indices into shared tables that a save reallocates, re-sorts or reloads.
    They stay valid only because one session at a time may edit socials or
    help, and one per combat-message slot. Keep those rules, add the same
    rule to anything else that rebuilds those tables (`reload xhelp` does not
    check), and check the index again before writing through it. In these
    editors `OLC_ZNUM` is not a zone, so code that shifts other sessions'
    `OLC_ZNUM` must keep selecting them by state, as
    `zedit.c:zedit_new_zone()` does.
30. `cedit` and `prefedit` write their whole snapshot back, so a save
    silently undoes every change made to the live data since the session
    opened, by another session or by the game. Copy back only what the
    builder changed, or refuse the concurrent change. `msgedit` also replaces
    a whole slot and is safe only while one session per slot is allowed.
31. Do not edit, free or reload a buffer that other code may still be
    reading. `page_string(..., 0)` keeps pointers into the text it pages
    between input lines, so `tedit` must not grow the live text globals in
    place and `hedit` must not free the help table under a reader. Edit a
    private copy, publish it whole, and never publish NULL.
32. On a successful save, `aedit` and `hedit` hand the scratch strings to the
    live table and must end with `CLEANUP_STRUCTS`. `cedit` and `msgedit`
    copy, so their scratch copy must still be freed (`cedit`'s never is). A
    workspace may hold a pointer to another character only if
    `free_char()` clears it, as `prefedit_forget_char()` does for
    `OLC_PREFS(d)->ch`.
33. Writers replace their file through a checked temporary file and
    `tb_replace_file_checked()` (`tedit` and `msgedit` do not), never call
    `exit()` from a save, tell the builder when the write failed, and agree
    with the loader on every key and reserved prefix: `cedit` writes
    `disp_closed_doors` where `load_config()` reads `display_closed_doors`,
    never writes `min_rent_cost`, and a message starting with `#` reads back
    as NULL in both the socials and the combat-message files.

## Pending changes on audit fix branches

None of the changes below is in this code yet. They are listed by branch,
using the branch that `audit-results-20261027/merge-plan.json` names for
each audit chunk; several fixes also exist as older duplicate commits on
other branches. Each entry was read in its commit; the commit messages give
the full reasoning and the tests.

**`audit-fix-F1`** (editors `redit`, `oedit`, `medit`, `zedit`, `sedit`;
new header `olc_checks.h`):

| Commit | Change |
| --- | --- |
| `9efcf47` | Each save captures the prototype's old trigger list before `add_*` overwrites it, and frees it afterwards. |
| `75bc7f0` | `OLC_SCRIPT` has one owner: a save hands it over and sets `OLC_SCRIPT(d)` to NULL; `cleanup_olc()` frees whatever is left (quit, `n`, failed save, lost link). |
| `91f18c6` | Deletes renumber every open session through one helper, `olc_renumber_sessions()`; a reference to the deleted entry is cancelled and reported. Display code checks scratch rnums with `OLC_CHECK_RNUM()` before indexing a table. |
| `197c134` | The `redit` insert path uses the same helper (never moves `NOWHERE`, adjusts only argument 1 of `'D'`/`'R'`). |
| `ee6d5ad` | `redit_save_internally()` decides insert or update with `real_room(OLC_NUM(d))`, so `W` copies and `rcopy` renumber other sessions. |
| `c14e20b` | `X` re-checks `can_edit_zone()` and that the scratch identity matches the edited vnum before deleting; `W` resets a copied mobile's scratch `nr`. |
| `5845db0` | `W` frees the scratch copy and trigger list it replaces, and only after the source was found. |
| `10b079a` | A stale `d->olc` found at session start is reported and released with `cleanup_olc()`. |
| `e8afd93` | `iedit` reports a vanished target without touching the freed pointer and does not announce a save. |
| `7b1d0af` | `iedit` takes the object out through the handler before writing and puts it back afterwards, so weight, applies, affect bits and light are re-accounted. |
| `f71950f` | `iedit` requires `can_edit_zone()` for the object's prototype zone (`LVL_GRGOD` for an object without a prototype). |
| `50f326f` | `delete_object()` frees the deleted prototype's strings and trigger list. |
| `45af63f` | `zedit` initialises new commands and gives the scratch list its own copies of `'V'` strings. |
| `bd6b81e` | Before `add_mobile()`, `medit_save_internally()` repairs a scratch `nr` that is not `real_mobile(OLC_NUM(d))` and reports `mob-proto-nr`, so a save can no longer write another prototype's rnum into the edited prototype and the mobiles loaded from it. |
| `020fc9b` | A new mobile renumbers other sessions through `olc_renumber_sessions()` (`'M'` commands, keepers, other `medit` scratch copies), skipping the saving session, descriptors without a workspace and `NOBODY`; new objects also renumber other `oedit` and `iedit` sessions. |
| `f682e6d` | Choosing a shop keeper only records it and requires `can_edit_zone()` for the keeper's zone; the save installs `shop_keeper`, keeps the keeper's previous procedure in `S_FUNC`, and gives a replaced keeper its procedure back unless another shop still uses it. |
| `5a60868` | The `sedit` room list shows a missing room as `<deleted room>` and leaves its vnum alone instead of rewriting it to 0, and object rnum 0 can be added as a product. |
| `647c4d3` | Deleting a zone command resets `OLC_VAL(d)` to 0, and an `A` abort whose remembered index lies outside the list is reported as `zedit-stale-index` (HEAD already ignores it, silently, since `5839b4c`). |
| `844ad6e` | The door-direction prompt lists only the directions the game uses and states the valid range (the `>= DIR_COUNT` check itself is already in HEAD, `38af36c`). |
| `b5358ef` | Menus range-check their name-table lookups (`genders[]`, `position_types[]`, `attack_hit_text[]`, `item_types[]` in `medit` and `sedit`; the `'E'` position and `'D'` direction in `zedit`), showing `<invalid>` and reporting a `table-index` error with the builder and vnum. |
| `ab75e06` | Save failures are reported: a failed `add_mobile()`, `add_object()` or `add_room()`, or a `zedit` save whose room no longer exists (`zedit-save`), is logged with builder and vnum, and the edit is neither announced nor written to disk as saved. |
| `fcdef0a` | Numeric prompts in `medit`, `sedit`, `oedit` and `zedit` refuse anything but a whole number; clamped values (load maxima, zone bottom and top, level limits, ...) are reported to the builder and logged as `olc-clamp`; the shop-flag and no-trade menus re-prompt instead of toggling the last flag. |
| `efa0ada` | The "should never get here" branches of `medit`, `sedit` and `zedit` close the session through `OLC_ABORT()`, which reports the bad mode or command, instead of using the freed workspace. |
| `51c7d0f` | A `medit` long description close to `MAX_INPUT_LENGTH` keeps its trailing `\r\n`, and `redit` refuses a room name of `MAX_ROOM_NAME` bytes or more. |

The F1 branch also has style- and comment-only commits (`be23b34`,
`5a15ebd`, `09e892f`, `1821c63`) that change no behaviour.

**`worktree-wf_c613c2c5-f9f-38`** (chunk F2: `cedit.c`, `aedit.c`, `hedit.c`,
`tedit.c`, `prefedit.c`, `msgedit.c`, `qedit.c`, and `interpreter.c` for
`sort_commands()`):

| Commit | Change |
| --- | --- |
| `9904311` | `prefedit`, `msgedit` and `qedit` return at once, reporting `olc-no-descriptor`, when the caller has no playing descriptor of its own; `msgedit` and `qedit` also refuse NPCs. |
| `f051ec4` | The entry commands of these editors report a leftover workspace (`olc-reentry`) and release it with `cleanup_olc(d, CLEANUP_ALL)` instead of `free()`; `cedit` now checks for one at all. |
| `14b9286` | The `cedit`, `aedit`, `hedit` and `qedit` save helpers return success, and every save path tells the builder when the file was not written. |
| `277c001` | Numeric prompts of `cedit` and `qedit` parse strictly against a range and re-prompt instead of storing 0 or clamping (`qedit` vnums must lie in -1..65534, so 68537 is refused instead of becoming 3001); `aedit` re-prompts for a command or sort word containing a space. |
| `cc82ad7` | `cedit`'s room-number prompts refuse anything outside the vnum range before `real_room()`, instead of truncating it to 16 bits. |
| `4f5b3cd` | `cedit` writes the configuration file after a save when `CONFIG_OLC_SAVE` is on, instead of testing the crash-save toggle `CONFIG_AUTO_SAVE`. |
| `624f880` | `cedit` frees the scratch IP address, data directory and log file name it replaces. |
| `5bb73dd` | `reassign_rooms()` covers the last room and clears only `dump` procedures before `assign_rooms()` runs. |
| `abb0c6c` | `aedit` re-prompts on an out-of-range position or level, and its menu and `astat` show `<invalid>` for a stored position outside `position_types[]` (positions overlap HEAD's `2eec6ee`). |
| `b68f5b8` | `aedit` refuses a social message that starts with `#`, which the socials file would read back as no message. |
| `9a9709d` | `sort_commands()` frees the previous `cmd_sort_info[]` before rebuilding it, so an `aedit` save no longer leaks it. |
| `56dff8a` | `hedit` refuses a setup or save whose index lies outside `help_table[]` and ends the session, where HEAD appends it as a new entry; "Help saved to disk." now follows the save (overlaps HEAD's `736ee87`). |
| `42a4acb` | `hedit <word>` refuses a search result outside the table and a duplicate keyword whose owning row cannot be found (overlaps HEAD's `d532d66`). |
| `b56f40b` | `hedit` no longer hands the string literal "" to the string editor, and an entry left NULL by `/c` gets a placeholder whose first line is the keyword. |
| `0381020` | `hedit` frees the replaced row's old strings after moving duplicate rows to the new text, refuses to save into a duplicate row, and its `N` walk offers only owning rows. |
| `6fdae42` | `hedit` logs saves with the correct `mudlog()` arguments and reports impossible modes with `TB_ERROR_MSG()`. |
| `71a4d7d` | `tedit` edits a private copy in `OLC_STORAGE` and replaces the global whole on save, publishing "" instead of NULL, so readers never see half-edited text. |
| `3164bc1` | `tedit` writes `<file>.tmp`, checks it and renames it over the file before replacing the live text, and tells the builder when a save fails. |
| `1fea157` | `prefedit` stores the edited player's idnum and looks the player up on every input line, ending the session once the player is gone (a different fix from HEAD's `prefedit_forget_char()`, `c5a3600`). |
| `9923061` | The same fix for `prefedit`'s "being edited" `act()` call as HEAD's `b6b0e7e`, with the buffer also initialised. |
| `3b504fe` | `msgedit` saves through `messages.tmp` and a rename, never calls `exit(1)`, and tells the builder when the write failed. |
| `9df53af` | `msgedit` keeps the quit intent in a per-session mode, `MSGEDIT_CONFIRM_SAVE_QUIT`, instead of a shared `static` flag. |
| `c777c60` | Deleting a quest that was never saved is refused with `qedit-delete-unsaved` and nothing is written; a failed `delete_quest()` is reported and skips the save. |
| `ef4d615` | A successful `qedit` save ends the session with `CLEANUP_ALL`, so the scratch quest's five strings are freed instead of leaked. |
| `62681f5` | The quest type is named through `qedit_type_name()`, which also reports a type outside the table (overlaps `b3e5b2c`, already in HEAD). |

F2 also has an include guard for `prefedit.h` (`f0a8b2f`), a macro rename
(`b0ec9d4`) and comment-only commits (`fbf212d`, `fe33225`, `4e55370`,
`6fc2826`, `fa13ad6`, `145415d`). Several F2 commits are different patches
for problems this branch already fixed (`56dff8a`/`736ee87`,
`42a4acb`/`d532d66`, `1fea157`/`c5a3600`, `abb0c6c`/`2eec6ee`,
`9923061`/`b6b0e7e`), so they will conflict when merged.

**`worktree-wf_c613c2c5-f9f-32`** (chunk C2: `genwld.c`, `genobj.c`,
`genzon.c`, `genshp.c`, `genqst.c`):

| Commit | Change |
| --- | --- |
| `7d4dd2f` | `add_room()` renumbers the reset commands of every zone; both `add_room()` and `delete_room()` renumber `GET_WAS_IN()`. |
| `90c3d54` | `copy_object()` and `copy_room()` keep the destination's running script and event list and free the trigger list they replace. |
| `cc089c0` | `delete_room()` shifts start rooms above the deleted rnum down. |
| `ae7aed7` | `add_room()` clears `people`, `contents`, `events` and `script` of an inserted room and reports a caller that passed any. |
| `22201d2` | `rebase_room_events()` points every room mud event back at its room after `world[]` moves. |
| `b552c7a` | `save_rooms()` writes a NULL extra-description field as empty instead of crashing. |
| `2752ad7` | Before a prototype update, each instance's script, private trigger list and restrung strings are released; instances keep their event list. |
| `49d1e01` | `delete_object()` drops shop products of the deleted prototype, refuses to delete a board's object, and leaves `NOTHING` board rnums alone; `save_shops()` abandons the zone save when a product rnum is past `top_of_objt`. |
| `ccb0728` | `delete_object()` rehomes contents by the container's real location and restarts its scan after each extraction. |
| `537e93c` | `delete_object()` no longer renumbers objects whose rnum is `NOTHING`. |
| `478050f` | `delete_object()` frees the prototype and refuses (instead of `assert()`) if instances remain. |
| `b201ab8` | `delete_object()`'s reset-command loop re-examines the slot after a deletion; dropped commands are reported and the zone is marked for saving. |
| `e480385` | `create_new_zone()` compares every zone, rejects overlapping ranges and creates files with `"wx"` so nothing is truncated. |
| `457fded` | `save_zone()` range-checks every stored rnum and abandons the save instead of reading out of bounds. |
| `e8e68ff` | `delete_quest()` checks its rnum, `NOTHING` included, before reading the table (`quest-delete-range`), and restores the questmaster's procedure through `real_mobile()`, reporting a questmaster without a prototype. |
| `b88ff58` | `save_shops()` abandons the zone save, reporting `shop-save-keeper`, when a keeper rnum is above `top_of_mobt`. |
| `882a5c0` | `save_quests()` abandons the zone save (`quest-save-size`) instead of dropping a record that does not fit and committing the rest. |
| `6cb1e83` | `create_world_index()` returns whether the index lists the zone and refuses an index with no `$` line; `save_shops()` and `save_quests()` return `FALSE`, keeping the zone on the save list, when it fails. |
| `45d10cb` | `save_shops()` reports open and header-write failures with path and errno and removes the partial `.new` file (with similar reports in `save_objects()` and `save_zone()`); `605da47` also reports a failed `remove()`. |

**`worktree-wf_c613c2c5-f9f-42`** (chunk C1: `genolc.c`, `genmob.c`):

| Commit | Change |
| --- | --- |
| `33c41e6` | `save_all()` visits each queued entry once, reports failures and frees skipped entries. |
| `d16c4f1` | Export writers check their writes; the info file counts the zone's exits itself. |
| `3b08462` | `delete_mobile()` frees the prototype instead of "extracting" it; doomed instances get `NOBODY` before the renumbering. |
| `8cb7eba` | `delete_mobile()` never renumbers `NOBODY`; a shop whose keeper is deleted gets `NOBODY` and a warning. |
| `a976562` | `delete_mobile()`'s reset-command loop re-examines the slot after a deletion. |
| `a54f56f` | `check_mobile_strings()` never indexes `mob_index[]` with `NOBODY`. |

**`worktree-wf_c613c2c5-f9f-39`** (chunk F3: `oasis.c`, `oasis.h`,
`oasis_copy.c`, `oasis_list.c`, `oasis_delete.c`):

| Commit | Change |
| --- | --- |
| `703f088` | `OLC_SCRIPT` single ownership, freed in `cleanup_olc()` (overlaps `75bc7f0`). |
| `3354104` | `cleanup_olc()` always frees `OLC_CONFIG`. |
| `8e5ea97` | `cleanup_olc()` validates its type and does not announce the end of a session that never left `CON_PLAYING` (copy, `dig`, `buildwalk`). |
| `03cface` | `dig`, `buildwalk` and the copy commands refuse when the descriptor already has an open editor. |
| `0fffc99` | `rcopy` saves as a new room (so sessions are renumbered); both vnums are range-checked instead of wrapping. |
| `0181384` | `rcopy` checks that the template shares no live list with the source room. |
| `8071c22` | `buildwalk` releases its template with `CLEANUP_ALL`, freeing the strings. |
| `6c1090a` | `dig` range-checks its target vnum. |
| `77c212d` | `mlist level` and `mlist flags` read prototypes instead of loading mobiles. |
| `0634fed` | `zedit`'s command copy owns its `sarg` strings (overlaps `45af63f`). |
| `30d15bb` | `close_socket()` releases the workspace of every OLC state, so `cedit`, `prefedit`, `ibtedit` and `iedit` sessions no longer leak on link loss. |

This branch also carries ten patch-identical copies of F2 commits
(`ebd240e`, `358111a`, `a36355f`, `6ecf1df`, `768009f`, `c650d73`,
`0363a7d`, `6e9aa4c`, `4e9ea3d`, `9279e27`), which `merge-plan.json` does
not list for F3.

**`worktree-wf_c613c2c5-f9f-22`**: `6e63025` makes a room trigger's `wait`
resume by room vnum instead of a pointer into `world[]` (see
[dg-scripts.md](dg-scripts.md#the-event-queue)).

**Base branch after `aeeb58d`.** `audit/macro-inventory-and-dg-followup-20261027`
has moved on since this document was written. These commits on it change
behaviour described above:

| Commit | Change |
| --- | --- |
| `1f9e24a` | `qedit`, `msgedit` and `prefedit` refuse a caller without a playing descriptor of its own (a forced linkless character or a switched body). |
| `2f25691` | `genzon.c:new_command()` starts a new zone command blank instead of leaving stack contents in its fields. |
| `3025fd8` | The "should never get here" branches of `medit`, `qedit` and `sedit` return after `cleanup_olc()` instead of using the freed workspace. |
| `03012f8` | The `W` copy of `oedit`, `medit` and `redit` frees the scratch copy and trigger list it replaces. |
| `922e5c0` | `prefedit`'s "restore defaults" sets the documented toggles and no longer switches off an immortal's no-hassle, holylight or show-vnums. |
| `beaca87` | `tedit` allows one editor per text file and refuses callers without a playing descriptor. |

## Further reading

- [README.md](README.md) and [overview.md](overview.md): how the server
  boots, the game loop and pulses.
- [networking.md](networking.md): descriptors, connection states, input and
  output.
- [persistence.md](persistence.md): how world files are loaded at boot and
  how player files are saved.
- [foundations.md](foundations.md): `utils.h` macros, `structs.h` and the
  error-reporting conventions.
- [dg-scripts.md](dg-scripts.md): the DG script engine, `trigedit` and
  trigger renumbering.
- [gameplay.md](gameplay.md): handler primitives, events, shops and quests
  at run time.
- [../maintainer-safety-guide.md](../maintainer-safety-guide.md): the
  diagnostic macros and lifetime rules every fix should follow.
- [../macro-audit.md](../macro-audit.md): audit history and the macro
  inventory.
- Legacy references: [../../doc/building.txt](../../doc/building.txt)
  (the builder's guide to world files and OLC) and
  [../../doc/msgedit.txt](../../doc/msgedit.txt).
