# Memory-safety audit, October 2026

A report for the maintainer. On 27 October 2026 every TBAMud source file was
read line by line for memory-safety bugs, silent failures and missing
diagnostics. A second, adversarial agent then tried to refute every
finding, running most of the serious ones under ASan, UBSan and Valgrind.
Fix agents fixed the confirmed ones, one commit per finding, on per-chunk
branches; follow-up rounds fixed what the chunk branches left open. All of
it is merged into `master` (see "How the fixes reached master").

The per-file and per-finding record is `docs/memory-safety-audit.md`, which
also describes the method in full. Every number and generated table in this
report comes from `tools/audit_report_stats.py` (see "Regenerating").

## At a glance

<!-- BEGIN GENERATED: report-glance -->

- **Scope:** 135 source files, 93,473 lines at the review base `18f80ce`, in 9 areas and 21 review chunks. 134 files were read line by line; `src/class.c` was not (see the ledger note).
- **Findings:** 523 (502 from the reviewers, 21 added by the verifiers). After verification: 442 confirmed, 43 plausible, 17 refuted.
- **Severity** of the 506 findings that stand: 16 critical, 103 high, 171 medium, 216 low.
- **Resolution:** 505 fixed, 0 partly fixed (the rest handed on), 0 deferred to follow-up groups, 0 queued for a later round, 1 skipped (no change needed), 17 refuted.
- **Commits:** 603 fix commits on 21 chunk branches, 430 of them with a regression test; plus the infrastructure and coordinator branches listed below.
- **Later rounds:** 74 of the findings above were fixed by the follow-up rounds after the chunk branches (they count as fixed).
- **Still open:** 0 findings, 0 of the 9 DG documentation findings. All 43 branches of the merge plans are integrated.

<!-- END GENERATED: report-glance -->

What matters most:

- **Every fix is merged.** The chunk branches, the follow-up rounds and
  the coordinator's direct fixes reached `master` through the integration
  branches and the two-phase master merge described below. No finding is
  left open; the one skipped finding needed no change.
- **One critical bug needed no account.** A2-01: two connections still at
  the name prompt could crash the server through the new-character
  duplicate check.
- **The most damaging live bug was not a memory error.** Since a mechanical
  `strcat` to `strlcat` conversion (9eefa23), `file_to_string()` bounded
  every text file by the size of a pointer. Files loaded as 7 bytes, and new
  players never saw the login prompt (B1-01). It was fixed directly and
  backported to the local branches, and `.clang-tidy` now rejects `sizeof`
  of a pointer.
- **Ordinary players could reach many of the critical bugs**: through rent
  and login (B2-01), bug reports and boards (C1-01, C1-02), a stock speech
  trigger (D1-01), furniture and fighting (H1-06) and quests (H3-01).
  Builders could reach most of the others through OLC.
- **DG scripts are the largest source of lifetime bugs.** A trigger purges
  its own object, a container or a sibling while the calling code still
  holds a pointer to it. The fixes re-validate the object, usually by UID,
  after each trigger call and before touching it again.
- **Corrupt or foreign data files** (world, player, rent, mail, house and
  board files) caused overflows, crashes at boot and silent data loss.

## Findings in numbers

<!-- BEGIN GENERATED: report-counts -->

**By verdict and severity** (523 findings; severity as re-rated by the verifier):

| Verdict | Findings | Critical | High | Medium | Low |
| --- | --- | --- | --- | --- | --- |
| confirmed | 442 | 16 | 95 | 159 | 172 |
| plausible | 43 | 0 | 0 | 3 | 40 |
| verifier-added | 21 | 0 | 8 | 9 | 4 |
| refuted (severity the reviewer claimed) | 17 | 1 | 6 | 4 | 6 |

**By severity and resolution** (refuted findings left out):

| Severity | Findings | Fixed | Partly fixed, rest in a group | Partly fixed, rest queued | Deferred to a group | Queued | Skipped |
| --- | --- | --- | --- | --- | --- | --- | --- |
| critical | 16 | 16 | 0 | 0 | 0 | 0 | 0 |
| high | 103 | 102 | 0 | 0 | 0 | 0 | 1 |
| medium | 171 | 171 | 0 | 0 | 0 | 0 | 0 |
| low | 216 | 216 | 0 | 0 | 0 | 0 | 0 |
| **Total** | 506 | 505 | 0 | 0 | 0 | 0 | 1 |

**By category:**

| Category | Findings | Critical | High | Medium | Low | Refuted | Fixed |
| --- | --- | --- | --- | --- | --- | --- | --- |
| buffer-bounds | 53 | 5 | 21 | 14 | 9 | 4 | 49 |
| integer | 29 | 1 | 2 | 17 | 9 | 0 | 29 |
| read-past-terminator | 20 | 0 | 4 | 7 | 8 | 1 | 19 |
| untrusted-index | 67 | 2 | 20 | 31 | 12 | 2 | 65 |
| lifetime | 54 | 6 | 27 | 9 | 10 | 2 | 52 |
| null-deref | 31 | 2 | 7 | 9 | 12 | 1 | 30 |
| double-free-or-leak | 42 | 0 | 3 | 13 | 26 | 0 | 42 |
| format-string | 8 | 0 | 0 | 1 | 3 | 4 | 4 |
| uninitialized | 13 | 0 | 1 | 7 | 5 | 0 | 13 |
| silent-failure | 110 | 0 | 8 | 41 | 59 | 2 | 108 |
| error-locality | 39 | 0 | 3 | 9 | 27 | 0 | 39 |
| undefined-behavior | 12 | 0 | 2 | 3 | 7 | 0 | 11 |
| sanitizer-hazard | 10 | 0 | 1 | 4 | 5 | 0 | 10 |
| other | 35 | 0 | 4 | 6 | 24 | 1 | 34 |

**By who controls the triggering input:**

| Input controller | Meaning | Findings | Critical | High | Medium | Low | Refuted |
| --- | --- | --- | --- | --- | --- | --- | --- |
| remote-unauthenticated | anyone, before login | 9 | 1 | 0 | 2 | 5 | 1 |
| player | any player | 55 | 6 | 12 | 20 | 14 | 3 |
| dg-script | DG script, written by a builder | 70 | 2 | 23 | 27 | 17 | 1 |
| builder-olc | builder, through OLC | 141 | 6 | 36 | 34 | 59 | 6 |
| save-or-world-file | world or save file | 119 | 0 | 22 | 62 | 33 | 2 |
| immortal | immortal | 44 | 1 | 6 | 14 | 21 | 2 |
| internal-invariant | no outside input | 85 | 0 | 4 | 12 | 67 | 2 |

**Fix commits on the chunk branches** (468 name finding IDs; 430 record a regression test):

| Kind (subject prefix) | Commits |
| --- | --- |
| security | 353 |
| diag | 91 |
| docs | 69 |
| style | 58 |
| fix | 19 |
| test | 13 |
| **Total** | 603 |

**Verifier runtime evidence** (reviewer findings; verifier severity):

| Severity | Verdicts | ASan, UBSan or Valgrind run | No sanitizer or Valgrind run |
| --- | --- | --- | --- |
| critical | 16 | 16 | 0 |
| high | 95 | 80 | 15 |
| medium | 162 | 98 | 64 |
| low | 229 | 47 | 182 |
| **Total** | 502 | 241 | 261 |

<!-- END GENERATED: report-counts -->

## The most serious bugs

Who can trigger a bug decides how urgent it is, so the tables run from the
most exposed input (anyone, before login) to the least (nobody: an internal
invariant). "Status" is the state on the fix branches; the ledger cites the
fix commits.

### Critical

<!-- BEGIN GENERATED: report-critical -->

| ID | Where | Who can trigger it | Impact | Status |
| --- | --- | --- | --- | --- |
| A2-01 | `src/interpreter.c:perform_new_char_dupe_check` | anyone, before login | Two connections that have not logged in crash the server: the new-character dupe check strcmp()s a NULL name. | fixed |
| B2-01 | `src/objsave.c:handle_obj` | any player | A worn container that auto-equip cannot put back on makes the login rent load dereference NULL. | fixed |
| C1-01 | `src/ibt.c:clean_ibt_list` | any player | Aborting one bug/idea/typo report frees another player's report while it is being written (use-after-free write). | fixed |
| C1-02 | `src/boards.c:board_write_message` | any player | 'write' with about 440 leading spaces at a board stores a NUL past the 512-byte input buffer (stack overflow). | fixed |
| D1-01 | `src/dg_variables.c:text_processed` | any player | Saying one 506-character word to a stock speech trigger overflows a 492-byte stack buffer in %speech.car%. | fixed |
| H1-06 | `src/fight.c:set_fighting / stop_fighting (with do_sit in act.movement.c:800)` | any player | Fighting while seated leaves the character on the old furniture; after sit and quit, the furniture keeps a freed character. | fixed |
| H3-01 | `src/quest.c:autoquest_trigger_check / generic_complete_quest` | any player | Completing a 'find mob' quest with two matching mobs present completes it twice; the second pass indexes the quest table with NOTHING. | fixed |
| B1-02 | `src/db.c:reset_zone` | DG script, written by a builder | A self-purging object load trigger frees the object inside reset_zone(), which keeps using it (heap use-after-free at boot or reset). | fixed |
| D1-06 | `src/dg_scripts.c:script_trigger_check` | DG script, written by a builder | A self-purging random or time object trigger frees the object while the trigger scan still walks it (use-after-free). | fixed |
| C2-01 | `src/genobj.c:delete_object` | builder, through OLC | Deleting an object whose reset is the zone's last command reads past the reset list and keeps scanning the heap. | fixed |
| C2-02 | `src/genobj.c:delete_object` | builder, through OLC | Deleting any object renumbers corpses and money to rnum 65534; extracting them later writes out of bounds. | fixed |
| C2-05 | `src/genqst.c:delete_quest` | builder, through OLC | Deleting a quest writes the questmaster's spec proc into mob_index[vnum] (heap out-of-bounds write). | fixed |
| E2-02 | `src/dg_olc.c:trigedit_save` | builder, through OLC | Creating a trigger leaves live copies of the displaced trigger on the wrong index entry; later saves free scripts still in use. | fixed |
| F2-01 | `src/hedit.c:hedit_parse / hedit_save_internally` | builder, through OLC | Answering 'n' to an hedit match and then adding a topic writes past the end of help_table. | fixed |
| F3-01 | `src/oasis_list.c:perform_obj_aff_list` | builder, through OLC | 'olist affect' over many long-named objects runs past the 49 KB stack buffer (size_t wrap). | fixed |
| G1-01 | `src/act.wizard.c:perform_set / do_set` | immortal | 'set file <player> room <vnum>' leaves the freed offline character in the room list (use-after-free on the next look). | fixed |

<!-- END GENERATED: report-critical -->

### High

<!-- BEGIN GENERATED: report-high -->

| ID | Where | Who can trigger it | Impact | Status |
| --- | --- | --- | --- | --- |
| A1-01 | `src/comm.c:process_output` | any player | A client that reads slowly can drive the output bookkeeping negative, so later output is written outside the buffer. | fixed |
| A1-02 | `src/comm.c:vwrite_to_output` | any player | A client that stops reading leaks one large output buffer each time the buffer fills (unbounded memory growth). | fixed |
| B1-04 | `src/db.c:fread_clean_string` | any player | A blank line in a bug, idea or typo body makes the next boot read one byte before a stack buffer. | fixed |
| B2-04 | `src/objsave.c:Crash_listrent` | any player | 'show rent' or 'hcontrol show' on a large rent or house file overflows a stack buffer. | fixed |
| B2-06 | `src/mail.c:postmaster_receive_mail` | any player | If the mail file cannot be rewritten, 'receive' creates mail objects forever until memory runs out. | fixed |
| C1-03 | `src/boards.c:board_remove_msg` | any player | Removing a board message leaves a duplicate index entry, which shutdown then frees twice. | fixed |
| D1-03 | `src/dg_scripts.c:eval_expr` | any player | eval substituted player text a second time, so a player could run DG variable calls (quest points, gold) by saying them. | fixed (on the audit branch) |
| D1-04 | `src/dg_variables.c:var_subst` | any player | An unterminated %variable makes var_subst() read stale stack bytes, which can be echoed to the room. | fixed |
| E1-04 | `src/dg_mobcmd.c:do_mload, do_mpurge, do_mforce, do_mhunt, do_mremember, do_mforget` | any player | With script_players on, mob commands run by a player character dereference a NULL desc->original. | fixed |
| F1-01 | `src/oedit.c:iedit_parse / iedit_save_internally / iedit_object_still_exists` | any player | Another player can junk an object that is being iedited; saving the edit then writes to the freed object. | fixed |
| G2-01 | `src/act.informative.c:search_help` | any player | 'help ~~~~' (any topic after the last keyword) reads past the end of the help table. | fixed |
| H2-06 | `src/handler.c:obj_from_room` | any player | Furniture taken by a scavenger or moved by a script keeps a pointer to its sitter, written after the sitter is freed. | fixed |
| D1-02 | `src/dg_scripts.c:process_wait` | DG script, written by a builder | 'wait' with a huge count overflows the event key into a negative bucket (heap write before the queue). | fixed |
| D1-07 | `src/dg_scripts.c:script_driver` | DG script, written by a builder | A script that frees its own owner indirectly (container purge, forced command) keeps running on the freed owner. | fixed |
| D2-01 | `src/dg_event.c:event_cancel / event_process` | DG script, written by a builder | An event cancelled from inside its own callback is freed while it runs (use-after-free). | fixed |
| D2-02 | `src/dg_event.c:event_create / queue_enq / queue_deq` | DG script, written by a builder | 'wait 9223372036854775807' overflows the event key and indexes the event queue out of bounds. | fixed |
| E1-02 | `src/dg_objcmd.c:do_oat` | DG script, written by a builder | 'oat <room> opurge self' frees the object do_oat() then reads; 'oat ... omove' leaves a permanent clone. | fixed |
| E1-03 | `src/dg_objcmd.c:do_opurge / do_oforce (script_driver owner contract)` | DG script, written by a builder | 'opurge <container>' frees the running object without telling the script driver (use-after-free on the next line). | fixed |
| E1-05 | `src/dg_objcmd.c:do_osetval` | DG script, written by a builder | 'osetval' stores any value, so a later drink or hit indexes a table out of bounds. | fixed |
| E1-06 | `src/dg_mobcmd.c:do_mtransform` | DG script, written by a builder | 'mtransform' on a seated mob leaves the chair pointing at the freed mob. | fixed |
| E1-07 | `src/dg_objcmd.c:do_otransform` | DG script, written by a builder | 'otransform' on occupied furniture leaves its sitters pointing at the freed object. | fixed |
| G1-05 | `src/act.wizard.c:do_return / do_switch` | DG script, written by a builder | 'force <linkdead player> return', or 'mforce all return', dereferences a NULL descriptor. | fixed |
| H1-01 | `src/act.item.c:get_from_room / get_from_container / do_put / do_drop / do_give / do_wear` | DG script, written by a builder | A get, drop, give or wear trigger that purges sibling objects makes 'get all' and friends follow freed pointers. | fixed |
| H1-02 | `src/act.item.c:perform_put / get_from_container` | DG script, written by a builder | A drop trigger that purges the container makes 'put' link the item into the freed container. | fixed |
| H1-03 | `src/act.movement.c:do_doorcmd` | DG script, written by a builder | A door trigger that removes the exit or purges the container makes the door command use NULL or freed memory. | fixed |
| H1-04 | `src/fight.c:hit` | DG script, written by a builder | A fight trigger that junks the attacker's weapon makes hit() read the freed weapon. | fixed |
| H1-05 | `src/fight.c:make_corpse` | DG script, written by a builder | A remove trigger that loads an object during death orphans it, leading to a use-after-free when it is extracted. | fixed |
| H1-07 | `src/act.item.c:do_drink / do_pour / name_from_drinkcon / name_to_drinkcon` | DG script, written by a builder | An out-of-range liquid type (osetval or world file) makes drink and pour index tables out of bounds. | fixed |
| H1-09 | `src/magic.c:mag_summons` | DG script, written by a builder | A zombie load trigger that purges the corpse makes animate dead read and free the corpse again. | fixed |
| H2-01 | `src/spell_parser.c:call_magic` | DG script, written by a builder | A cast trigger that purges its object makes call_magic() use the freed object. | fixed |
| H2-02 | `src/spell_parser.c:mag_objectmagic` | DG script, written by a builder | A cast trigger that purges the scroll or potion makes it be extracted twice (double free). | fixed |
| H2-03 | `src/spell_parser.c:mag_objectmagic` | DG script, written by a builder | An out-of-range spell number on a wand or staff indexes spell_info[] out of bounds. | fixed |
| H2-V01 | `src/spell_parser.c:mag_objectmagic` | DG script, written by a builder | A cast trigger that purges the staff makes the staff loop read the freed object on its next pass. | fixed |
| H3-08 | `src/limits.c:point_update` | DG script, written by a builder | A timer trigger that purges the next object makes point_update() follow a freed pointer. | fixed |
| I1-02 | `src/utils.c:stop_follower / die_follower` | DG script, written by a builder | An act trigger that forces 'unfollow' during stop_follower() makes it dereference a NULL master. | fixed |
| A1-07 | `src/comm.c:msdp_update` | builder, through OLC | A mob with 0d0+0 hit dice has max_hit 0, and fighting it divides by zero in the MSDP update (SIGFPE). | fixed |
| C1-07 | `src/boards.c:init_boards` | builder, through OLC | A missing board object makes the first 'look board' call exit(1) mid-game, losing unsaved progress. | fixed |
| C1-20 | `src/genolc.c:save_all` | builder, through OLC | One failed OLC save makes 'saveall', and the save at shutdown, loop forever. | fixed |
| C1-21 | `src/genmob.c:delete_mobile` | builder, through OLC | Deleting a mob whose reset is the zone's last command reads past the reset list (the C2-01 bug in genmob.c). | fixed |
| C1-V01 | `src/genmob.c:delete_mobile` | builder, through OLC | Deleting a mob renumbers keeper-less shops to keeper 65534; saves and shop listings then read mob_index out of bounds. | fixed |
| C1-V03 | `src/genobj.c:delete_object` | builder, through OLC | The reset-list over-read of C1-21 in the object deleter (the same defect as C2-01). | fixed |
| C2-03 | `src/genwld.c:add_room / delete_room` | builder, through OLC | Saving a new room reallocates world[]; darkness events and other stored room pointers are left dangling. | fixed |
| C2-04 | `src/genwld.c:add_room` | builder, through OLC | A copied or new room shares the source room's object list; picking objects up later corrupts both rooms. | fixed |
| C2-07 | `src/genobj.c:delete_object` | builder, through OLC | Deleting a container prototype strands the contents of live containers outside every list. | fixed |
| C2-08 | `src/genobj.c:delete_object` | builder, through OLC | Deleting the highest object leaves shop products one past the object table (heap over-read on save and restock). | fixed |
| C2-09 | `src/genshp.c:save_shops` | builder, through OLC | save_shops() reads mob_index out of bounds for a keeper renumbered to 65534. | fixed |
| C2-V01 | `src/genwld.c:delete_room` | builder, through OLC | Deleting a room below the start room silently moves the mortal, immortal and frozen start rooms. | fixed |
| D1-08 | `src/dg_scripts.c:find_case` | builder, through OLC | A switch or while with no 'done' makes find_case() dereference NULL. | fixed |
| E2-01 | `src/dg_olc.c:format_script` | builder, through OLC | trigedit's /f formatter overruns a 256-entry stack array on deeply nested blocks. | fixed |
| E2-03 | `src/dg_olc.c:trigedit_save` | builder, through OLC | Copying a trigger over another in trigedit gives the prototype the wrong index; later saves free scripts still in use. | fixed |
| F1-02 | `src/redit.c:redit_setup_existing` | builder, through OLC | A redit copy shares the source room's object list with the new room (use-after-free later). | fixed |
| F1-03 | `src/oedit.c:oedit_parse (OEDIT_DELETE) / medit_parse (MEDIT_DELETE) / redit_parse (REDIT_DELETE)` | builder, through OLC | Deleting an object, mob or room leaves other builders' open editors indexing past the shrunken tables. | fixed |
| F1-04 | `src/oedit.c:oedit_parse (OEDIT_COPY/OEDIT_DELETE), medit_parse (MEDIT_COPY/MEDIT_DELETE), redit_parse (REDIT_COPY/REDIT_DELETE)` | builder, through OLC | Copy then delete in oedit, medit or redit removes a prototype, with its live instances, from a zone the builder may not edit. | fixed |
| F1-05 | `src/oedit.c:oedit_save_internally` | builder, through OLC | Inserting a new object shifts other oedit sessions' numbers, so their delete removes the wrong prototype. | fixed |
| F2-02 | `src/hedit.c:do_oasis_hedit` | builder, through OLC | 'hedit <new topic>' reads help_table[65535]. | fixed |
| F2-03 | `src/hedit.c:hedit_parse` | builder, through OLC | Clearing a help entry and editing it again frees a string literal (invalid free). | fixed |
| F2-08 | `src/qedit.c:qedit_parse (QEDIT_CONFIRM_DELETE) -> delete_quest` | builder, through OLC | Deleting a quest that was never saved calls delete_quest(NOTHING). | fixed |
| F2-09 | `src/aedit.c:aedit_parse (AEDIT_MIN_CHAR_POS/AEDIT_MIN_VICT_POS)` | builder, through OLC | aedit stores any position number; any player's 'astat' then reads out of bounds, and the value is saved to disk. | fixed |
| F3-02 | `src/oasis_list.c:perform_obj_aff_list` | builder, through OLC | 'olist affect' reads one past the object table and follows a wild pointer. | fixed |
| F3-03 | `src/oasis_list.c:perform_obj_type_list` | builder, through OLC | 'olist type <n>' indexes item_types[] with an unchecked number. | fixed |
| G2-02 | `src/act.informative.c:do_score` | builder, through OLC | A player on a deleted quest crashes the server with 'score'. | fixed |
| H1-10 | `src/magic.c:mag_rooms` | builder, through OLC | Darkness keeps a raw world[] pointer for 5 seconds; an OLC room save in that window leaves it dangling. | fixed |
| H2-04 | `src/handler.c:fname` | builder, through OLC | A keyword of 256 or more letters (an exit keyword, say) overflows fname()'s static buffer. | fixed |
| H2-05 | `src/shop.c:push` | builder, through OLC | A shop namelist of 101 or more terms overflows the expression stack when an item is sold or valued. | fixed |
| H3-02 | `src/quest.c:generic_complete_quest` | builder, through OLC | A quest whose reward object is also its target recurses until the stack overflows. | fixed |
| H3-03 | `src/quest.c:quest_list / quest_stat` | builder, through OLC | Deleting a quest named as another quest's prev or next makes 'quest list' read aquest_table[65535]. | fixed |
| H3-13 | `src/mud_event.c:event_countdown / attach_mud_event / free_mud_event` | builder, through OLC | The dangling world[] pointer of H1-10, seen from mud_event.c. | fixed |
| H3-15 | `src/mud_event.c:free_mud_event (EVENT_ROOM/EVENT_CHAR) with free_list` | builder, through OLC | Cancelling the last room or character event NULLs the list under the caller; free_list(NULL) then crashes. | fixed |
| H3-V01 | `src/genqst.c:169` | builder, through OLC | delete_quest() writes the questmaster's spec pointer into mob_index[vnum] (the C2-05 defect). | fixed |
| I1-01 | `src/utils.c:column_list` | builder, through OLC | A social name of 60 or more characters plus a wide screen makes 'socials' overflow column_list()'s buffer. | fixed |
| B1-03 | `src/db.c:setup_dir` | world file | A 'D' direction outside 0-9 in a world file writes a heap pointer over other room fields (silent corruption). | fixed |
| B1-05 | `src/db.c:fread_string` | world file | A world-file line of 512 or more characters writes past a stack buffer in fread_string(). | fixed |
| B2-02 | `src/players.c:load_char` | save file | An over-long Act, Aff or Pref line in a player file overflows 128-byte stack buffers at login. | fixed |
| B2-03 | `src/players.c:tag_argument` | save file | A 1-3 character line in a player or rent file is read past its end. | fixed |
| B2-05 | `src/mail.c:read_mail_record / read_delete` | save file | One malformed mail header makes the next 'receive' delete every later player's mail. | fixed |
| B2-07 | `src/mail.c:read_mail_record` | save file | A mail file cut off mid-append makes the server exit(1) at boot and on every mail check. | fixed |
| B2-08 | `src/house.c:House_boot` | save file | A corrupt or foreign hcontrol record (guest count) makes house commands read and write past guests[]. | fixed |
| B2-09 | `src/house.c:House_boot` | save file | Houses that fail validation at boot are dropped from hcontrol for good, with no log line. | fixed |
| B2-10 | `src/players.c:build_player_index` | save file | One malformed player-index line hides every later player (they become new characters) and loses their mail and houses. | fixed |
| C1-04 | `src/boards.c:board_load_board` | save file | A corrupt board file supplies raw pointers and slot numbers that are freed and used as indexes. | fixed |
| C2-13 | `src/genwld.c:save_rooms` | world file | An empty extra-description body loads as NULL, and the next save of that zone crashes the server. | fixed |
| G2-03 | `src/act.informative.c:show_obj_to_char / list_obj_to_char / list_one_char / find_exdesc` | world file | An object or mob with an empty description crashes 'look' (NULL dereference). | fixed |
| H1-08 | `src/fight.c:hit / dam_message / replace_string` | world file | A weapon type out of range indexes attack_hit_text past its end and overflows a global buffer. | fixed |
| I2-02 | `src/util/plrtoascii.c:plr_filename` | save file (offline tool) | plrtoascii writes a 78-byte name into a 40-byte buffer; names like '../../x' leave the output directory. | fixed |
| I2-03 | `src/util/plrtoascii.c:convert` | save file (offline tool) | plrtoascii prints an uninitialised buffer as the first record's name; later records get wrong names. | fixed |
| I2-05 | `src/util/rebuildAsciiIndex.c:walkdir` | save file (offline tool) | rebuildAsciiIndex writes 6 fields per record; the server rejects the index and every character becomes unknown. | fixed |
| I2-V02 | `src/util/rebuildAsciiIndex.c:walkdir` | save file (offline tool) | rebuildAsciiIndex also writes the wrong level into the index. | fixed |
| I3-01 | `src/util/wld2html.c:461` | world file (offline tool) | wld2html: a 'D' direction outside 0-9 writes past a room's exit array. | fixed |
| I3-02 | `src/util/wld2html.c:545` | world file (offline tool) | wld2html: a line of 512 or more characters overflows a stack buffer; a leading NUL underflows it. | fixed |
| I3-03 | `src/util/wld2html.c:423` | world file (offline tool) | wld2html: a '#' that the 128-byte chunk reader misses is parsed but not counted (heap overflow). | fixed |
| I3-04 | `src/util/wld2html.c:566` | world file (offline tool) | wld2html: a false strlcpy() bound aborts every run under _FORTIFY_SOURCE and hides overflows from the sanitizers. | fixed |
| I3-V01 | `src/util/wld2html.c:566` | world file (offline tool) | wld2html: I3-02 and I3-04 together give a real heap overflow in ordinary builds. | fixed |
| C1-13 | `src/ibt.c:do_ibt` | immortal | 'bug remove' frees a report another player is still writing, or makes an open ibtedit save over a different report. | fixed |
| C2-12 | `src/genzon.c:create_new_zone` | immortal | 'zedit new' on the highest zone overwrites that zone's seven world files with empty stubs, silently. | fixed |
| F2-06 | `src/prefedit.c:prefedit_setup / prefedit_disp_main_menu / prefedit_save_to_char` | immortal | 'prefedit <player>' keeps a pointer to a player who then quits; saving writes to freed memory. | fixed |
| F3-05 | `src/oasis_copy.c:do_oasis_copy` | immortal | 'rcopy' shares the source room's object list with the copy (use-after-free after a 'junk'). | fixed |
| G1-04 | `src/act.wizard.c:do_stat` | immortal | 'stat file <player>' deletes the player's crash file and can overwrite an online player's pfile. | fixed |
| H3-11 | `src/asciimap.c:str_and_map / MapArea` | immortal | A minimap size above 11 (cedit or config) makes the automap read and write outside its arrays. | fixed |
| B1-01 | `src/db.c:file_to_string` | always: every text-file load | Every text file (greeting, motd, news, help pages) loaded as its first 7 bytes, so new players got no login prompt. | fixed |
| B2-11 | `src/players.c:save_player_index` | a failed disk write | A failed player-index write goes unnoticed; an empty index then makes the next new character an implementor. | fixed |
| B2-12 | `src/objsave.c:Crash_rentsave / Crash_crashsave / Crash_idlesave / Crash_cryosave` | a failed disk write | When a rent or crash file cannot be written, the player's inventory is lost and nothing is logged. | fixed |
| I2-01 | `src/util/autowiz.c:get_line` | always: every autowiz run | autowiz cut every player-index line to 7 bytes, so the wizlist and immlist were always empty. | fixed |

Left out: A1-V01 (rated high, but the fix agent found the code already safe).

<!-- END GENERATED: report-high -->

### Found outside the chunk review

The coordinator fixed these directly. Several are the same bug as a chunk
finding whose fix was still on its branch while the bug was live. The
new-player login fix is also on the local branches as `security: new players
can log in again (login hotfix backport)`, and the two server binaries in
use were rebuilt with it.

<!-- BEGIN GENERATED: report-extra -->

| Bug | Found by | Who or what triggers it | Fix (commit subject) | Where the fix is | Chunk finding | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Text files loaded as their first 7 bytes: no login prompt for new players | new players could not log in; the B1 reviewer had also reported it (B1-01) | every new connection | `security: text files load whole again, not as their first 7 bytes` | this branch | B1-01 | adds the WarnOnSizeOfPointer clang-tidy error and tests/test_login_greeting.py; backported as `security: new players can log in again (login hotfix backport)` to 1 local branches (master: unknown) |
| autowiz read player-index lines as their first 7 bytes | the sizeof-of-pointer sweep; the I2 reviewer had also reported it (I2-01) | every autowiz run (empty wizlist and immlist) | `security: autowiz reads whole index lines, not their first 7 bytes` | this branch | I2-01 | - |
| New player-index slots written to disk uninitialised; a reused slot leaked its name | Valgrind on a live server | every new character | `security: new player-index slots start zeroed, not as realloc garbage` | this branch | - | - |
| Host string leaked once per new character | Valgrind on a live server | every new character | `security: creating a character no longer leaks its host string` | this branch | - | - |
| search_help() read one entry past the help table ('help ~~~~') | the cloud review; the G2 reviewer had also reported it (G2-01) | any player | `security: help on a topic past the last keyword no longer over-reads` | this branch | G2-01 | adds tests/test_help_search.py |
| BOOT-UAF: world teardown left world[] dangling for the DG debugging scans | circle_boot_check (circle -c) under ASan and Valgrind | every clean shutdown and every circle -c | `security: world teardown leaves no dangling world pointer (BOOT-UAF)`<br>`fix: shutdown no longer reads the freed world (regression from 4f33b6c)` | audit-fix-infra-findings; this branch has `security: world teardown leaves no dangling world pointer (BOOT-UAF)` | D2-05 | re-enables circle_boot_check |
| FUZZ-1: ProtocolOutput() read one byte past text ending in a tab | fuzz_protocol (libFuzzer); the same loop as A1-06 | any output that ends in a tab | `security: protocol output never reads past a trailing tab; MSSP callbacks typed` | this branch | A1-06 | - |
| FUZZ-2: MSSP callbacks called through a mismatched function type | fuzz_protocol (libFuzzer) | any MSSP request, no login needed | `security: protocol output never reads past a trailing tab; MSSP callbacks typed` | this branch | - | - |
| vwrite_to_output() wrote a size_t length through an int pointer | the sanitizer-infrastructure review (also A1-10) | every line of output | `security: protocol output length is written through a real int` | this branch | A1-10 | - |
| Parallel builds overwrote one another's bin/circle | the coordinator, before the fix agents started | tooling | `build: give each CMake build tree its own bin directory` | this branch | - | - |
| No SAST in CI | the coordinator | tooling | `ci: run Semgrep SAST and upload to the Semgrep AppSec Platform` | this branch | - | - |

<!-- END GENERATED: report-extra -->

## New tooling

- **Sanitizer options** (`build: sanitizer builds trap more UB and share
  runtime options with CI`): extra UBSan checks (float-cast-overflow,
  float-divide-by-zero, local-bounds, nullability), an opt-in
  `SANITIZER_STRICT_INTEGER`, and `TBAMUD_ASAN_OPTIONS` /
  `TBAMUD_UBSAN_OPTIONS` (stack use after return, init order, strict string
  checks, ODR, halt on the first error) applied to every ctest test, the
  same locally and in CI.
- **Hardening build** (`build: ENABLE_HARDENING option and -Wformat=2`):
  `_FORTIFY_SOURCE=3`, stack protector, stack-clash protection,
  `-fcf-protection`, pattern auto-initialisation and full RELRO. It is
  refused together with the sanitizers. `-Wformat=2` is on everywhere.
- **Valgrind in CTest and CI** (`test: Valgrind memcheck configuration and a
  whole-world boot test`, `ci: Valgrind memcheck and hardened-release
  jobs`): `ctest -T memcheck` with definite and indirect leaks counted as
  errors, test labels, `circle_boot_check` (`circle -c` on the whole world
  under the sanitizers), and CI jobs for memcheck and the hardened release
  build.
- **Allocator and buffer-pool poisoning** (`build: zmalloc poisons freed
  blocks and guard pads under ASan`, `build: pooled output buffers are
  poisoned under ASan`): the custom allocators no longer hide
  use-after-free from ASan, and zmalloc reports double frees and pad
  corruption as structured errors.
- **Fuzzing** (`test: libFuzzer target for the telnet/protocol layer`):
  `ENABLE_FUZZING` builds `fuzz_protocol`. Its seed corpus and the two crash
  inputs it found run as ctest regression tests.
- **Error-locality checks** (`ci: error-locality audit catches more raw
  reporting and ratchets legacy`): `tools/check_error_locality.sh` forbids
  raw writes to stderr and operation names that are not kebab-case, and
  ratchets the legacy SYSERR logging counts so they can only fall.
- **clang-tidy sizeof-of-pointer rule:** `.clang-tidy` makes
  `bugprone-sizeof-expression` with `WarnOnSizeOfPointer` an error (part of
  `security: text files load whole again, not as their first 7 bytes`).
- **End-to-end tests** that boot the real server on a free port:
  `tests/test_login_greeting.py` requires the login name prompt, and
  `tests/test_help_search.py` requires `help ~~~~` to be refused with no
  sanitizer report.
- **Semgrep CI** (`ci: run Semgrep SAST and upload to the Semgrep AppSec
  Platform`, branch `audit/semgrep-ci`).
- **One `bin/` per build tree** (`build: give each CMake build tree its own
  bin directory`), so parallel builds no longer overwrite each other's
  server binary.
- **This report's generator**, `tools/audit_report_stats.py`.

The infrastructure pass's proposals, and the commit that implemented each:

<!-- BEGIN GENERATED: report-infra -->

| Proposal | Priority | Commit (subject) or status |
| --- | --- | --- |
| Stricter sanitizer checks, with the same runtime options locally and in CI | high | `build: sanitizer builds trap more UB and share runtime options with CI` |
| ENABLE_HARDENING option and -Wformat=2 | medium | `build: ENABLE_HARDENING option and -Wformat=2` |
| Valgrind memcheck in CTest, test labels, and a whole-world boot test | high | `test: Valgrind memcheck configuration and a whole-world boot test` |
| CI jobs for Valgrind memcheck and a hardened release build | high | `ci: Valgrind memcheck and hardened-release jobs` |
| zmalloc: ASan poisoning, alignment, a fixed calloc macro, and loud failures | high | `build: zmalloc poisons freed blocks and guard pads under ASan`<br>`test: test_zmalloc and zmalloc.h are clean under clang-tidy` |
| Poison pooled output buffers in comm.c's bufpool | medium | `build: pooled output buffers are poisoned under ASan` |
| More rules in check_error_locality.sh | medium | `ci: error-locality audit catches more raw reporting and ratchets legacy` |
| libFuzzer harness for the telnet/protocol parser | medium | `test: libFuzzer target for the telnet/protocol layer` |
| Fix BOOT-UAF, FUZZ-1 and FUZZ-2 in game code, then remove DISABLED | high | not implemented: game-logic files are outside this task's allowed edit set; done afterwards by the coordinator: `security: world teardown leaves no dangling world pointer (BOOT-UAF)`<br>`security: protocol output never reads past a trailing tab; MSSP callbacks typed` |
| Fuzz the world and player file loaders | medium | not implemented: too large and risky for this pass; circle_boot_check covers the loaders against real data once BOOT-UAF is fixed |
| Time-boxed fuzzing in CI and a MEMORY_DEBUG+ASan CI job | low | not implemented: blocked on the game-code fixes |
| Sanitizer and hardening switches for the autoconf build | low | not implemented: lower priority than the CMake/CI work |

<!-- END GENERATED: report-infra -->

## How the fixes reached master

1. **Chunk and follow-up branches.** Each chunk fix agent worked on its own
   `audit-fix-<chunk>` branch. Findings that needed files outside the chunk,
   and the queued non-memory findings, went to follow-up groups that fixed
   them in later rounds on `audit-deferred-<group>` branches: db-loaders
   (rounds 1-5), player-load (1-3), dg, gen-olc (1-2), core-utils (1-2),
   commands, errloc and cedit (1-4), plus the leftovers (1-2), world-data
   (1-2), followups, stranded and verify-static branches.
2. **Integration.** Integrators cherry-picked the plans in the results
   directory's `merge-plan*.json` onto `audit/full-hardening-integration`
   (A) and `-b`; A merged B and was tagged `audit-merge-ab-green`
   (ddd51b6). The later branches went onto `-c` through `-c8`, which fanned
   in to `-m1` (c3, c5, c8 and the cedit/e2e work) and `-m2` (c4, c6, c7).
   Where a chunk fix and a coordinator fix covered the same bug (B1-01,
   I2-01, G2-01, A1-06, A1-10), one of the two was kept. The fan-in also
   fixed the bugs merging exposed, such as the double free of a replaced
   trigger list in the OLC saves (0076760) and `VALID_*_RNUM` accepting
   negative rnums (M2's `tb_rnum_valid`).
3. **Phase 1.** `audit/master-merge-20261028`, cut from `origin/master`,
   merged the audit branch, the tag and `audit/verify-static`, and was
   pushed to `master` as 2c180c9b (fast-forward).
4. **Phase 2.** The same branch then merged the three fan-in branches,
   `audit/full-hardening-integration-c` (DG and followups),
   `-m2` and `-m1`, with the hand resolutions recorded in the results
   directory's `master-merge-notes.md`, and re-enabled the remaining fuzz
   regression inputs.

## What remains open

- **No finding is open.** The follow-up rounds fixed every deferred,
  partly fixed and queued finding, the nine DG documentation findings and
  the memory-safety observations the fix agents recorded outside their
  scope (tables below; each cites its fix commit). A few non-memory
  observations remain marked "not fixed".
- **Semgrep needs a token.** Add the repository secret `SEMGREP_APP_TOKEN`
  (a Semgrep CI token), then re-run the failed Semgrep job. Without the
  token the job fails on purpose rather than passing without a scan. Pull
  requests from forks get no secrets and are skipped.
- **`src/class.c` was not read line by line.** Its reviewer filtered out the
  plain `case N: return M;` rows of the level tables between lines 800 and
  2170 and read everything else (see the ledger note).
- **The world and player-file loaders have no fuzz target yet.**

### New DG findings from the documentation pass

<!-- BEGIN GENERATED: report-dg-docs -->

| Label | Finding (as reported) | Severity | Fix owner | Status |
| --- | --- | --- | --- | --- |
| DGDOC-1 | dg_variables.c:find_replacement(), `%x.level(N)%` setter: no target check — any builder script can lower an immortal's level (privilege change by builder input). | high | dg follow-up | fixed: `security: a DG level setter never changes an immortal's level` |
| DGDOC-2 | dg_scripts.c:do_attach(): for mobs/objects checks the zone of the builder's own room, not the target's zone (detach checks the target). Builder authority bypass. | not rated | dg follow-up | fixed: `security: attach and detach check the zone of the trigger's target` |
| DGDOC-3 | dg_scripts.c:do_vdelete(): any builder can delete any player's saved script variables by UID; no zone/level check. | not rated | dg follow-up | fixed: `security: vdelete checks the builder's zone and rank before deleting` |
| DGDOC-4 | script_driver() owner check: misses a player owner being extracted; a script can run for a mob already pending extraction (wait expiring in the same pulse the mob died). Lifetime. | not rated | dg follow-up | fixed: `security: a mob script never runs for a character pending extraction` |
| DGDOC-5 | dg_triggers.c:damage_mtrigger(): the script's return value becomes the damage and the default is 1, so a damage trigger without `return %damage%` reduces every hit to 1. | not rated | dg follow-up | fixed: `fix: a mob damage trigger without 'return' keeps the damage` |
| DGDOC-6 | script_driver(): control keywords matched by prefix (`else` matches `elsewhere`, `end` matches `endure`); `while` counters live in the command list shared by all instances of a trigger. | not rated | dg follow-up | fixed: `fix: DG control keywords match whole words, the same way in every scanner`<br>`fix: the DG while-loop pause counts each trigger instance's own passes` |
| DGDOC-7 | medit_save_internally()/oedit_save_internally() (owner: gen-olc agent): every save rebuilds the scripts of all live copies, wiping their variables and pending waits even when the trigger list did not change. | not rated | gen-olc agent | fixed: `fix: oedit and medit keep live scripts when the trigger list is unchanged` |
| DGDOC-8 | dg_comm.c:sub_write(): visibility-dependent object lookup is inverted (dead code today; owner: errloc or later round). | not rated | errloc or later round | fixed: `fix: sub_write's visible-object lookup tries room, equipment, inventory` |
| DGDOC-9 | Lookup table: an object ID equal to the bucket sentinel (1,000,000,000) could never be found (theoretical). | not rated | dg follow-up | fixed: `fix: an object UID equal to the lookup sentinel is stored where lookups look` |

Merge note from the same file: cfa23ca/eed9ae3 (C2) keep missing-trigger T lines; dg_olc.c:dg_script_menu() then needs -28's checked lookup merged too, or it reads past the trigger index.

<!-- END GENERATED: report-dg-docs -->

### Open findings, plausible findings and loose ends

<!-- BEGIN GENERATED: report-open -->

**Follow-up groups** (all merged; "Fixed" counts chunk-branch and later-round fixes):

| Group | Branch | Based on | Files owned | Findings | Still partly open | Not started | Fixed | Refuted |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `commands` | - | `audit-fix-G1` | 5 | 8 | 0 | 0 | 8 | 0 |
| `core-utils` | - | `audit-fix-I2` | 11 | 13 | 0 | 0 | 13 | 0 |
| `db-loaders` | - | `worktree-wf_c613c2c5-f9f-30` | 2 | 8 | 0 | 0 | 8 | 0 |
| `dg` | - | `worktree-wf_c613c2c5-f9f-33` | 10 | 12 | 0 | 0 | 12 | 0 |
| `errloc` | - | `worktree-wf_c613c2c5-f9f-28` | 7 | 9 | 0 | 0 | 9 | 0 |
| `gen-olc` | - | `worktree-wf_c613c2c5-f9f-32` | 11 | 15 | 0 | 0 | 14 | 1 |
| `player-load` | - | `worktree-wf_c613c2c5-f9f-31` | 3 | 7 | 0 | 0 | 7 | 0 |

**Open findings per group** (none, when every group is done):

| Group | Partly fixed (rest open) | Not started |
| --- | --- | --- |
| `commands` | - | - |
| `core-utils` | - | - |
| `db-loaders` | - | - |
| `dg` | - | - |
| `errloc` | - | - |
| `gen-olc` | - | - |
| `player-load` | - | - |

**Queued for a later round** (14 findings, not memory safety, in files the follow-up groups own):

| ID | File | Category | Severity | Kind | Status |
| --- | --- | --- | --- | --- | --- |
| D2-06 | `src/dg_handler.c` | silent-failure | low | plausible | fixed |
| D2-08 | `src/dg_handler.c` | silent-failure | low | plausible | fixed |
| D2-11 | `src/dg_handler.c` | error-locality | low | plausible | fixed |
| I1-04 | `src/utils.c` | silent-failure | medium | deferred | fixed |
| I1-10 | `src/utils.c` | error-locality | low | deferred | fixed |
| I1-14 | `src/utils.c` | silent-failure | low | deferred | fixed |
| F3-20 | `src/oasis.c` | other | low | plausible | fixed |
| C2-16 | `src/genobj.c` | silent-failure | medium | plausible | fixed |
| C1-29 | `src/genmob.c` | silent-failure | low | plausible | fixed |
| E2-15 | `src/dg_olc.c` | error-locality | low | plausible | fixed |
| G2-18 | `src/act.informative.c` | silent-failure | low | deferred | fixed |
| G2-20 | `src/act.informative.c` | other | low | plausible | fixed |
| D1-27 | `src/dg_scripts.c` | silent-failure | low | deferred | fixed |
| I2-26 | `src/sysdep.h` | silent-failure | low | deferred | fixed |

**Plausible findings not fixed yet:** 0 of 43 (43 were fixed); every one of them is scheduled in a follow-up group or the queue.

| Scheduled in | Findings | IDs |
| --- | --- | --- |

**Observations the fix agents recorded outside their scope** (not findings; a later commit fixed some of them):

| Chunk | Observation | What the fix agent wrote | Status |
| --- | --- | --- | --- |
| A2 | oos:utils.c count_color_chars | Out of scope (src/utils.c, line 1334). A string ending in a lone '\t' gives i += 2, then the while test reads string[len+1], one byte past the terminator. Callers include act.other.c:345 (GET_NAME) and medit.c:495. format_text no longer passes it such a string, but the function itself still needs fixing by the utils.c owner. | fixed: `security: count_color_chars stops at a lone trailing tab` |
| A2 | oos:interpreter.c perform_new_char_dupe_check leak | Not fixed; it was not a listed finding, and the fix changes descriptor lifetime. In the 'boot the older one' branch, k->character is set to NULL without free_char(), so close_socket() leaks that pre-creation char_data. Suggested fix: leave k->character attached and set STATE(k)=CON_CLOSE so close_socket() frees it. This needs a socket-lifecycle test first. | fixed: `security: the new-character dupe check frees the characters it boots` |
| A2 | oos:protocol.c ProtocolCreate | Out of scope (protocol.c; only protocol.h is in this chunk). The malloc() results for pVariables and each MSDP_t are used without a NULL check, so get_protocols' precondition holds only while allocation succeeds. | fixed: `security: protocol allocations abort on failure instead of writing through NULL` |
| A2 | oos:improved-edit.c escaped \r\n | Minor, user-visible, not a listed finding. parse_edit_action PARSE_FORMAT prints "Invalid line range.\\r\\n" and "That range is invalid.\\r\\n" with doubled backslashes, so the player sees a literal \r\n. Left unchanged. | fixed: `fix: editor range errors end with a line break, not a literal \r\n` |
| A2 | oos:ban.c site case | Documented, not changed. load_banned() keeps file sites as written, but isbanned() lowercases the host, so an uppercase site in a hand-edited BAN_FILE never matches. A follow-up could lowercase sites on load and emit a warning. | fixed: `fix: ban file sites are stored lowercased, so uppercase ones match` |
| B2 | pre-existing: tests/test_warning_bounds_contracts.py test_objsave_rent_headers_are_checked | Fails at the audit base 1e03b0c as well: it expects 3 objsave_parse_rent_header(line, calls and finds 4. It is not registered with ctest and is unrelated to these commits, so it was left alone. | fixed: `test: rent-header contract checks every Crash_* header reader` |
| C2 | NEW: add_cmd_to_list pos > count | Not an assigned finding, reported for triage. add_cmd_to_list (genzon.c) does not check pos. zedit_save_internally (zedit.c:303-335) advances subcmd past G/E/P commands it refuses to save, so a later command's pos can exceed the live list's count. That command is then silently not inserted. Example: a room whose only other zone commands are [G (no mob), O] loses the O. | fixed: `fix: zedit saves keep every reset command reset_zone accepts, in place` |
| H1 | NEW: perform_move follower loop | Not in the findings list, so documented only (7d39533). perform_move saves k->next before perform_move(follower). The follower's leave/entry/greet triggers can reach stop_follower() on another follower of ch, which frees that follow_type node, and the loop then follows a dangling pointer. Suggested fix: snapshot follower UIDs and re-acquire each one with find_char() plus an IN_ROOM check. | fixed: `security: followers are moved from a snapshot, not a list triggers free` |

**Infrastructure proposals still not implemented:**

| Proposal | Priority | Reason the infrastructure pass gave |
| --- | --- | --- |
| Fuzz the world and player file loaders | medium | too large and risky for this pass; circle_boot_check covers the loaders against real data once BOOT-UAF is fixed |
| Time-boxed fuzzing in CI and a MEMORY_DEBUG+ASan CI job | low | blocked on the game-code fixes |
| Sanitizer and hardening switches for the autoconf build | low | lower priority than the CMake/CI work |

<!-- END GENERATED: report-open -->

## Regenerating

The inputs are the JSON files in the audit results directory. From the
repository root:

```sh
python3 -I tools/audit_report_stats.py --results <results dir>          # headline numbers
python3 -I tools/audit_report_stats.py --results <results dir> --write  # regenerate both documents
python3 -I tools/audit_report_stats.py --results <results dir> --check  # are they current?
```

The script also reads git, without changing anything: the line count at the
review base, whether a coordinator fix is already on this branch, the
subjects of the infrastructure commits, and the number of login-hotfix
backports.
