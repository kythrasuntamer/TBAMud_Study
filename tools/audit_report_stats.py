#!/usr/bin/env python3
"""Numbers and tables for the October 2026 memory-safety audit.

Every count and generated table in docs/memory-safety-audit.md and
docs/audit-2026-10-report.md comes from this script. It reads the audit
workflow's results directory:

  reviews/<chunk>.json    one reviewer per chunk (A1..I3): files_read with
                          fully_read flags, findings, macro_proposals, doc_gaps
  verify/<chunk>.json     the adversarial verifier: one verdict per finding,
                          plus missed_findings it added (IDs <chunk>-Vnn)
  fixes/<chunk>.json      the fix agent: branch, commits (finding_ids,
                          test_added), skipped findings with reasons
  fixes/sanitizer-infra.json        infrastructure proposals and commits
  deferred/<group>.json             follow-up groups (findings handed on)
  deferred/queued-nonmemory-in-owned-files.json   queued for a later round
  deferred/new-findings-from-dg-docs.md           open DG findings (markdown)
  merge-plan*.json, integrate-state*.json         integration (optional; the
                          plan is split between two integration branches)

The repository (--repo, default ".") is only read, with git: line counts at
the review base, whether an extra fix is already on HEAD, the subjects of
the infrastructure commits, and how many login-hotfix backports exist.

Usage, from the repository root:
  python3 -I tools/audit_report_stats.py --results DIR           # headlines
  python3 -I tools/audit_report_stats.py --results DIR --json    # all counts
  python3 -I tools/audit_report_stats.py --results DIR --write   # update docs
  python3 -I tools/audit_report_stats.py --results DIR --check   # stale docs?

--write replaces the text between "<!-- BEGIN GENERATED: name -->" and
"<!-- END GENERATED: name -->" in the two documents and leaves the prose
around them alone.

A few classifications cannot be read off one JSON field. They are the
curated tables below (SAME_DEFECT_AS, FIXED_EARLIER, SKIP_REASON,
REFUTED_REASON, ALSO_FIXED_BY, IMPACT, EXTRA_FIXES); each entry is a
summary of the fix agent's skip reason, the verifier's evidence or a commit
message, and is used for wording only, never to change a count by itself.
"""

import argparse
import collections
import glob
import json
import os
import re
import subprocess
import sys

DEFAULT_RESULTS = os.environ.get(
    "TB_AUDIT_RESULTS", "/home/daero-local/TBAMud_Test/audit-results-20261027")
REVIEW_BASE = "18f80ce"   # the commit the chunk reviewers read
LEDGER = os.path.join("docs", "memory-safety-audit.md")
REPORT = os.path.join("docs", "audit-2026-10-report.md")
HOTFIX_SUBJECT = "security: new players can log in again (login hotfix backport)"
QUEUED_FILE = "queued-nonmemory-in-owned-files.json"
DG_DOCS_FILE = "new-findings-from-dg-docs.md"

# The audit scope: the 135 files of docs/memory-safety-audit.md, by area.
AREAS = [
    ("A", "Untrusted network input",
     "comm.c comm.h protocol.c protocol.h telnet.h interpreter.c interpreter.h ban.c ban.h modify.c modify.h improved-edit.c improved-edit.h"),
    ("B", "Player and world persistence",
     "db.c db.h players.c pfdefaults.h objsave.c house.c house.h mail.c mail.h"),
    ("C", "Boards, bug reports, class import, OLC save writers, trigger files, config",
     "boards.c boards.h ibt.c ibt.h class.c class.h genolc.c genolc.h genmob.c genmob.h genobj.c genobj.h genwld.c genwld.h genzon.c genzon.h genshp.c genshp.h genqst.c dg_db_scripts.c config.c config.h"),
    ("D", "DG script engine core",
     "dg_scripts.c dg_scripts.h dg_variables.c dg_misc.c dg_handler.c dg_event.c dg_event.h"),
    ("E", "DG script commands, triggers and trigger OLC",
     "dg_mobcmd.c dg_objcmd.c dg_wldcmd.c dg_comm.c dg_triggers.c dg_olc.c dg_olc.h"),
    ("F", "OLC editors",
     "oedit.c medit.c redit.c zedit.c sedit.c cedit.c aedit.c hedit.c hedit.h qedit.c tedit.c prefedit.c prefedit.h msgedit.c msgedit.h oasis.c oasis.h oasis_copy.c oasis_delete.c oasis_list.c"),
    ("G", "Player and immortal commands",
     "act.h act.wizard.c act.informative.c act.comm.c act.social.c act.other.c"),
    ("H", "Gameplay and world mechanics",
     "act.item.c act.movement.c act.offensive.c fight.c fight.h magic.c spell_parser.c spells.c spells.h handler.c handler.h shop.c shop.h quest.c quest.h castle.c spec_assign.c spec_procs.c spec_procs.h mobact.c limits.c weather.c graph.c graph.h asciimap.c asciimap.h mud_event.c mud_event.h lists.c lists.h"),
    ("I", "Foundations (utilities, macros, core headers) and standalone tools",
     "utils.c utils.h structs.h sysdep.h screen.h constants.c constants.h random.c bsd-snprintf.c bsd-snprintf.h zmalloc.c zmalloc.h util/asciipasswd.c util/autowiz.c util/plrtoascii.c util/rebuildAsciiIndex.c util/rebuildMailIndex.c util/shopconv.c util/sign.c util/split.c util/wld2html.c"),
]
AREAS = [(a, name, ["src/" + f for f in files.split()]) for a, name, files in AREAS]

ID_RE = re.compile(r"^([A-I])([1-3])-(V?)(\d+)$")
SEVERITIES = ["critical", "high", "medium", "low"]
VERDICTS = ["confirmed", "plausible", "verifier-added", "refuted"]
CATEGORIES = ["buffer-bounds", "integer", "read-past-terminator", "untrusted-index",
              "lifetime", "null-deref", "double-free-or-leak", "format-string",
              "uninitialized", "silent-failure", "error-locality",
              "undefined-behavior", "sanitizer-hazard", "other"]
CONTROLLERS = ["remote-unauthenticated", "player", "dg-script", "builder-olc",
               "save-or-world-file", "immortal", "internal-invariant"]

# Resolution classes, in table order.
FIXED = "fixed"
PART_DEFERRED = "partly fixed, rest in a follow-up group"
PART_QUEUED = "partly fixed, rest queued for a later round"
DEFERRED = "deferred to a follow-up group"
QUEUED = "queued for a later round"
SKIPPED = "skipped, no change needed"
REFUTED = "refuted"
RESOLUTIONS = [FIXED, PART_DEFERRED, PART_QUEUED, DEFERRED, QUEUED, SKIPPED, REFUTED]
OPEN = {PART_DEFERRED, PART_QUEUED, DEFERRED, QUEUED}
HAS_BRANCH_FIX = {FIXED, PART_DEFERRED, PART_QUEUED}

# --- Curated wording (see the module docstring) ---------------------------

# fixes/F2.json skips F2-12: "The OLC-core chunk already fixed it" with
# 'security: always release the cedit config copy in cleanup_olc', which is
# F3's commit for F3-07 (the same cleanup_olc defect).
SAME_DEFECT_AS = {"F2-12": "F3-07"}

# fixes/D1.json skips D1-03 as "Already fixed on the audit branch tip by
# a195541"; that commit is an ancestor of the audit branch.
FIXED_EARLIER = {
    "D1-03": ("a195541",
              "security: player text never becomes DG variable syntax on a second pass"),
}

# fixes/A1.json skip reason for A1-V01, shortened. The guard dates from 58ae5ed.
SKIP_REASON = {
    "A1-V01": "already safe: hitprcnt_mtrigger() tests GET_MAX_HIT(ch) before "
              "dividing, and that guard predates the audit (the verifier missed it)",
}

# The verifiers' evidence for each refuted finding, shortened.
_SPLITTERS = ("already fixed by `security: bound the argument splitters to their "
              "MAX_INPUT_LENGTH contract` (05425e1)")
_SHOPMSG = ("already fixed by `security: expand shop messages without printf; "
            "type-check shop file reads` (02400df)")
REFUTED_REASON = {
    "A1-03": "already fixed by `security: \"--\" flushes only queued input, not the "
             "live output buffer` (35a2832)",
    "A1-21": "no caller can pass an invalid rnum (act.movement.c checks to_room; "
             "mud_event.c indexes world[] first); a guard would be defence in depth only",
    "A1-23": "both TO_GMOTE callers pass a non-NULL ch, and a playing character is "
             "never in NOWHERE between handler calls",
    "A1-31": "every caller passes an eMSDP_* constant; client MSDP input is resolved "
             "by name through validated loops",
    "A2-04": "no reachable string is long enough for strncpy() to leave the buffer "
             "unterminated (editor input is capped far below MAX_STRING_LENGTH)",
    "B1-17": _SPLITTERS,
    "B1-19": _SPLITTERS,
    "B1-20": _SPLITTERS + "; the silent tb_parse_int_or_zero() it mentions is a "
             "separate low issue",
    "B1-26": "create_entry() runs just before init_char() and cannot fail; the else "
             "branch also logs a SYSERR",
    "C1-27": "process_input() keeps only printable ASCII and splits lines, so the "
             "argument cannot forge a log record; the command is LVL_IMPL only",
    "C2-06": _SHOPMSG,
    "E1-01": "already fixed by `security: DG log commands never treat script or name "
             "text as a format` (6e3be0c)",
    "F1-06": _SHOPMSG,
    "F2-05": "already fixed by `security: send client-reported protocol banner as "
             "data, not a format` (91bad16)",
    "G1-02": _SPLITTERS,
    "H1-11": "call_magic() returns before mag_rooms() in a NOMAGIC room, so the "
             "darkness bit is never set there; the check is dead code",
    "I1-03": "the duplicate free/assign pair is not in this tree (the reviewer read "
             "another checkout); only a silent out-of-range return remains, listed "
             "in the gen-olc follow-up",
}

# Chunk findings whose bug was also fixed directly by the coordinator, because
# the chunk fix was still on its branch while the bug was live.
ALSO_FIXED_BY = {
    "B1-01": "security: text files load whole again, not as their first 7 bytes",
    "I2-01": "security: autowiz reads whole index lines, not their first 7 bytes",
    "G2-01": "security: help on a topic past the last keyword no longer over-reads",
    "A1-10": "security: protocol output length is written through a real int",
    "A1-06": "security: protocol output never reads past a trailing tab; MSSP callbacks typed",
}

# One-line impact of every critical and high finding, from the reviewer's
# failure scenario and the verifier's runtime check.
IMPACT = {
    # critical
    "A2-01": "Two connections that have not logged in crash the server: the new-character dupe check strcmp()s a NULL name.",
    "B1-02": "A self-purging object load trigger frees the object inside reset_zone(), which keeps using it (heap use-after-free at boot or reset).",
    "B2-01": "A worn container that auto-equip cannot put back on makes the login rent load dereference NULL.",
    "C1-01": "Aborting one bug/idea/typo report frees another player's report while it is being written (use-after-free write).",
    "C1-02": "'write' with about 440 leading spaces at a board stores a NUL past the 512-byte input buffer (stack overflow).",
    "C2-01": "Deleting an object whose reset is the zone's last command reads past the reset list and keeps scanning the heap.",
    "C2-02": "Deleting any object renumbers corpses and money to rnum 65534; extracting them later writes out of bounds.",
    "C2-05": "Deleting a quest writes the questmaster's spec proc into mob_index[vnum] (heap out-of-bounds write).",
    "D1-01": "Saying one 506-character word to a stock speech trigger overflows a 492-byte stack buffer in %speech.car%.",
    "D1-06": "A self-purging random or time object trigger frees the object while the trigger scan still walks it (use-after-free).",
    "E2-02": "Creating a trigger leaves live copies of the displaced trigger on the wrong index entry; later saves free scripts still in use.",
    "F2-01": "Answering 'n' to an hedit match and then adding a topic writes past the end of help_table.",
    "F3-01": "'olist affect' over many long-named objects runs past the 49 KB stack buffer (size_t wrap).",
    "G1-01": "'set file <player> room <vnum>' leaves the freed offline character in the room list (use-after-free on the next look).",
    "H1-06": "Fighting while seated leaves the character on the old furniture; after sit and quit, the furniture keeps a freed character.",
    "H3-01": "Completing a 'find mob' quest with two matching mobs present completes it twice; the second pass indexes the quest table with NOTHING.",
    # high
    "A1-01": "A client that reads slowly can drive the output bookkeeping negative, so later output is written outside the buffer.",
    "A1-02": "A client that stops reading leaks one large output buffer each time the buffer fills (unbounded memory growth).",
    "A1-07": "A mob with 0d0+0 hit dice has max_hit 0, and fighting it divides by zero in the MSDP update (SIGFPE).",
    "A1-V01": "Claimed: the HitPrcnt trigger check divides by a zero max_hit. The guard already exists.",
    "B1-01": "Every text file (greeting, motd, news, help pages) loaded as its first 7 bytes, so new players got no login prompt.",
    "B1-03": "A 'D' direction outside 0-9 in a world file writes a heap pointer over other room fields (silent corruption).",
    "B1-04": "A blank line in a bug, idea or typo body makes the next boot read one byte before a stack buffer.",
    "B1-05": "A world-file line of 512 or more characters writes past a stack buffer in fread_string().",
    "B2-02": "An over-long Act, Aff or Pref line in a player file overflows 128-byte stack buffers at login.",
    "B2-03": "A 1-3 character line in a player or rent file is read past its end.",
    "B2-04": "'show rent' or 'hcontrol show' on a large rent or house file overflows a stack buffer.",
    "B2-05": "One malformed mail header makes the next 'receive' delete every later player's mail.",
    "B2-06": "If the mail file cannot be rewritten, 'receive' creates mail objects forever until memory runs out.",
    "B2-07": "A mail file cut off mid-append makes the server exit(1) at boot and on every mail check.",
    "B2-08": "A corrupt or foreign hcontrol record (guest count) makes house commands read and write past guests[].",
    "B2-09": "Houses that fail validation at boot are dropped from hcontrol for good, with no log line.",
    "B2-10": "One malformed player-index line hides every later player (they become new characters) and loses their mail and houses.",
    "B2-11": "A failed player-index write goes unnoticed; an empty index then makes the next new character an implementor.",
    "B2-12": "When a rent or crash file cannot be written, the player's inventory is lost and nothing is logged.",
    "C1-03": "Removing a board message leaves a duplicate index entry, which shutdown then frees twice.",
    "C1-04": "A corrupt board file supplies raw pointers and slot numbers that are freed and used as indexes.",
    "C1-07": "A missing board object makes the first 'look board' call exit(1) mid-game, losing unsaved progress.",
    "C1-13": "'bug remove' frees a report another player is still writing, or makes an open ibtedit save over a different report.",
    "C1-20": "One failed OLC save makes 'saveall', and the save at shutdown, loop forever.",
    "C1-21": "Deleting a mob whose reset is the zone's last command reads past the reset list (the C2-01 bug in genmob.c).",
    "C1-V01": "Deleting a mob renumbers keeper-less shops to keeper 65534; saves and shop listings then read mob_index out of bounds.",
    "C1-V03": "The reset-list over-read of C1-21 in the object deleter (the same defect as C2-01).",
    "C2-03": "Saving a new room reallocates world[]; darkness events and other stored room pointers are left dangling.",
    "C2-04": "A copied or new room shares the source room's object list; picking objects up later corrupts both rooms.",
    "C2-07": "Deleting a container prototype strands the contents of live containers outside every list.",
    "C2-08": "Deleting the highest object leaves shop products one past the object table (heap over-read on save and restock).",
    "C2-09": "save_shops() reads mob_index out of bounds for a keeper renumbered to 65534.",
    "C2-12": "'zedit new' on the highest zone overwrites that zone's seven world files with empty stubs, silently.",
    "C2-13": "An empty extra-description body loads as NULL, and the next save of that zone crashes the server.",
    "C2-V01": "Deleting a room below the start room silently moves the mortal, immortal and frozen start rooms.",
    "D1-02": "'wait' with a huge count overflows the event key into a negative bucket (heap write before the queue).",
    "D1-03": "eval substituted player text a second time, so a player could run DG variable calls (quest points, gold) by saying them.",
    "D1-04": "An unterminated %variable makes var_subst() read stale stack bytes, which can be echoed to the room.",
    "D1-07": "A script that frees its own owner indirectly (container purge, forced command) keeps running on the freed owner.",
    "D1-08": "A switch or while with no 'done' makes find_case() dereference NULL.",
    "D2-01": "An event cancelled from inside its own callback is freed while it runs (use-after-free).",
    "D2-02": "'wait 9223372036854775807' overflows the event key and indexes the event queue out of bounds.",
    "E1-02": "'oat <room> opurge self' frees the object do_oat() then reads; 'oat ... omove' leaves a permanent clone.",
    "E1-03": "'opurge <container>' frees the running object without telling the script driver (use-after-free on the next line).",
    "E1-04": "With script_players on, mob commands run by a player character dereference a NULL desc->original.",
    "E1-05": "'osetval' stores any value, so a later drink or hit indexes a table out of bounds.",
    "E1-06": "'mtransform' on a seated mob leaves the chair pointing at the freed mob.",
    "E1-07": "'otransform' on occupied furniture leaves its sitters pointing at the freed object.",
    "E2-01": "trigedit's /f formatter overruns a 256-entry stack array on deeply nested blocks.",
    "E2-03": "Copying a trigger over another in trigedit gives the prototype the wrong index; later saves free scripts still in use.",
    "F1-01": "Another player can junk an object that is being iedited; saving the edit then writes to the freed object.",
    "F1-02": "A redit copy shares the source room's object list with the new room (use-after-free later).",
    "F1-03": "Deleting an object, mob or room leaves other builders' open editors indexing past the shrunken tables.",
    "F1-04": "Copy then delete in oedit, medit or redit removes a prototype, with its live instances, from a zone the builder may not edit.",
    "F1-05": "Inserting a new object shifts other oedit sessions' numbers, so their delete removes the wrong prototype.",
    "F2-02": "'hedit <new topic>' reads help_table[65535].",
    "F2-03": "Clearing a help entry and editing it again frees a string literal (invalid free).",
    "F2-06": "'prefedit <player>' keeps a pointer to a player who then quits; saving writes to freed memory.",
    "F2-08": "Deleting a quest that was never saved calls delete_quest(NOTHING).",
    "F2-09": "aedit stores any position number; any player's 'astat' then reads out of bounds, and the value is saved to disk.",
    "F3-02": "'olist affect' reads one past the object table and follows a wild pointer.",
    "F3-03": "'olist type <n>' indexes item_types[] with an unchecked number.",
    "F3-05": "'rcopy' shares the source room's object list with the copy (use-after-free after a 'junk').",
    "G1-04": "'stat file <player>' deletes the player's crash file and can overwrite an online player's pfile.",
    "G1-05": "'force <linkdead player> return', or 'mforce all return', dereferences a NULL descriptor.",
    "G2-01": "'help ~~~~' (any topic after the last keyword) reads past the end of the help table.",
    "G2-02": "A player on a deleted quest crashes the server with 'score'.",
    "G2-03": "An object or mob with an empty description crashes 'look' (NULL dereference).",
    "H1-01": "A get, drop, give or wear trigger that purges sibling objects makes 'get all' and friends follow freed pointers.",
    "H1-02": "A drop trigger that purges the container makes 'put' link the item into the freed container.",
    "H1-03": "A door trigger that removes the exit or purges the container makes the door command use NULL or freed memory.",
    "H1-04": "A fight trigger that junks the attacker's weapon makes hit() read the freed weapon.",
    "H1-05": "A remove trigger that loads an object during death orphans it, leading to a use-after-free when it is extracted.",
    "H1-07": "An out-of-range liquid type (osetval or world file) makes drink and pour index tables out of bounds.",
    "H1-08": "A weapon type out of range indexes attack_hit_text past its end and overflows a global buffer.",
    "H1-09": "A zombie load trigger that purges the corpse makes animate dead read and free the corpse again.",
    "H1-10": "Darkness keeps a raw world[] pointer for 5 seconds; an OLC room save in that window leaves it dangling.",
    "H2-01": "A cast trigger that purges its object makes call_magic() use the freed object.",
    "H2-02": "A cast trigger that purges the scroll or potion makes it be extracted twice (double free).",
    "H2-03": "An out-of-range spell number on a wand or staff indexes spell_info[] out of bounds.",
    "H2-04": "A keyword of 256 or more letters (an exit keyword, say) overflows fname()'s static buffer.",
    "H2-05": "A shop namelist of 101 or more terms overflows the expression stack when an item is sold or valued.",
    "H2-06": "Furniture taken by a scavenger or moved by a script keeps a pointer to its sitter, written after the sitter is freed.",
    "H2-V01": "A cast trigger that purges the staff makes the staff loop read the freed object on its next pass.",
    "H3-02": "A quest whose reward object is also its target recurses until the stack overflows.",
    "H3-03": "Deleting a quest named as another quest's prev or next makes 'quest list' read aquest_table[65535].",
    "H3-08": "A timer trigger that purges the next object makes point_update() follow a freed pointer.",
    "H3-11": "A minimap size above 11 (cedit or config) makes the automap read and write outside its arrays.",
    "H3-13": "The dangling world[] pointer of H1-10, seen from mud_event.c.",
    "H3-15": "Cancelling the last room or character event NULLs the list under the caller; free_list(NULL) then crashes.",
    "H3-V01": "delete_quest() writes the questmaster's spec pointer into mob_index[vnum] (the C2-05 defect).",
    "I1-01": "A social name of 60 or more characters plus a wide screen makes 'socials' overflow column_list()'s buffer.",
    "I1-02": "An act trigger that forces 'unfollow' during stop_follower() makes it dereference a NULL master.",
    "I2-01": "autowiz cut every player-index line to 7 bytes, so the wizlist and immlist were always empty.",
    "I2-02": "plrtoascii writes a 78-byte name into a 40-byte buffer; names like '../../x' leave the output directory.",
    "I2-03": "plrtoascii prints an uninitialised buffer as the first record's name; later records get wrong names.",
    "I2-05": "rebuildAsciiIndex writes 6 fields per record; the server rejects the index and every character becomes unknown.",
    "I2-V02": "rebuildAsciiIndex also writes the wrong level into the index.",
    "I3-01": "wld2html: a 'D' direction outside 0-9 writes past a room's exit array.",
    "I3-02": "wld2html: a line of 512 or more characters overflows a stack buffer; a leading NUL underflows it.",
    "I3-03": "wld2html: a '#' that the 128-byte chunk reader misses is parsed but not counted (heap overflow).",
    "I3-04": "wld2html: a false strlcpy() bound aborts every run under _FORTIFY_SOURCE and hides overflows from the sanitizers.",
    "I3-V01": "wld2html: I3-02 and I3-04 together give a real heap overflow in ordinary builds.",
}

# Bugs fixed directly by the coordinator, outside the chunk workflow.
EXTRA_FIXES = [
    dict(bug="Text files loaded as their first 7 bytes: no login prompt for new players",
         found="new players could not log in; the B1 reviewer had also reported it (B1-01)",
         trigger="every new connection",
         subjects=["security: text files load whole again, not as their first 7 bytes"],
         branch="audit-fix-sizeof-pointer", related="B1-01", backport=True,
         notes="adds the WarnOnSizeOfPointer clang-tidy error and tests/test_login_greeting.py"),
    dict(bug="autowiz read player-index lines as their first 7 bytes",
         found="the sizeof-of-pointer sweep; the I2 reviewer had also reported it (I2-01)",
         trigger="every autowiz run (empty wizlist and immlist)",
         subjects=["security: autowiz reads whole index lines, not their first 7 bytes"],
         branch="audit-fix-sizeof-pointer", related="I2-01"),
    dict(bug="New player-index slots written to disk uninitialised; a reused slot leaked its name",
         found="Valgrind on a live server",
         trigger="every new character",
         subjects=["security: new player-index slots start zeroed, not as realloc garbage"],
         branch="audit-fix-newchar-valgrind", related=""),
    dict(bug="Host string leaked once per new character",
         found="Valgrind on a live server",
         trigger="every new character",
         subjects=["security: creating a character no longer leaks its host string"],
         branch="audit-fix-newchar-valgrind", related=""),
    dict(bug="search_help() read one entry past the help table ('help ~~~~')",
         found="the cloud review; the G2 reviewer had also reported it (G2-01)",
         trigger="any player",
         subjects=["security: help on a topic past the last keyword no longer over-reads"],
         branch="audit-fix-cloud-review", related="G2-01",
         notes="adds tests/test_help_search.py"),
    dict(bug="BOOT-UAF: world teardown left world[] dangling for the DG debugging scans",
         found="circle_boot_check (circle -c) under ASan and Valgrind",
         trigger="every clean shutdown and every circle -c",
         subjects=["security: world teardown leaves no dangling world pointer (BOOT-UAF)",
                   "fix: shutdown no longer reads the freed world (regression from 4f33b6c)"],
         branch="audit-fix-infra-findings", related="D2-05",
         notes="re-enables circle_boot_check"),
    dict(bug="FUZZ-1: ProtocolOutput() read one byte past text ending in a tab",
         found="fuzz_protocol (libFuzzer); the same loop as A1-06",
         trigger="any output that ends in a tab",
         subjects=["security: protocol output never reads past a trailing tab; MSSP callbacks typed"],
         branch="audit-fix-infra-findings", related="A1-06"),
    dict(bug="FUZZ-2: MSSP callbacks called through a mismatched function type",
         found="fuzz_protocol (libFuzzer)",
         trigger="any MSSP request, no login needed",
         subjects=["security: protocol output never reads past a trailing tab; MSSP callbacks typed"],
         branch="audit-fix-infra-findings", related=""),
    dict(bug="vwrite_to_output() wrote a size_t length through an int pointer",
         found="the sanitizer-infrastructure review (also A1-10)",
         trigger="every line of output",
         subjects=["security: protocol output length is written through a real int"],
         branch="audit-fix-infra-findings", related="A1-10"),
    dict(bug="Parallel builds overwrote one another's bin/circle",
         found="the coordinator, before the fix agents started",
         trigger="tooling",
         subjects=["build: give each CMake build tree its own bin directory"],
         branch="the audit branch", related=""),
    dict(bug="No SAST in CI",
         found="the coordinator",
         trigger="tooling",
         subjects=["ci: run Semgrep SAST and upload to the Semgrep AppSec Platform"],
         branch="audit/semgrep-ci", related=""),
]

# Infrastructure proposals the infrastructure pass could not implement
# (game code was outside its edit set) that the coordinator did afterwards.
INFRA_DONE_LATER = {
    "Fix BOOT-UAF, FUZZ-1 and FUZZ-2 in game code": [
        "security: world teardown leaves no dangling world pointer (BOOT-UAF)",
        "security: protocol output never reads past a trailing tab; MSSP callbacks typed",
    ],
}

CONTROLLER_LABEL = {
    "remote-unauthenticated": "anyone, before login",
    "player": "any player",
    "dg-script": "DG script, written by a builder",
    "builder-olc": "builder, through OLC",
    "save-or-world-file": "world or save file",
    "immortal": "immortal",
    "internal-invariant": "no outside input",
}
# Where "who controls the input" undersells how often a bug fired.
TRIGGER_OVERRIDE = {
    "B1-01": "always: every text-file load",
    "I2-01": "always: every autowiz run",
    "B2-11": "a failed disk write",
    "B2-12": "a failed disk write",
}
SAVE_FILE_SOURCES = {"src/players.c", "src/objsave.c", "src/house.c", "src/mail.c",
                     "src/boards.c", "src/ibt.c", "src/util/plrtoascii.c",
                     "src/util/rebuildAsciiIndex.c", "src/util/rebuildMailIndex.c",
                     "src/util/autowiz.c", "src/util/asciipasswd.c"}


# --- Small helpers ----------------------------------------------------------

def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def hand_resolved_notes(state, state_path, problems):
    """(sha, note) for each entry of an integrate-state file's hand_resolved.

    The integrators wrote three shapes: [sha, note] pairs,
    {"sha": ..., "note": ...} objects and "sha note" strings (sha first,
    possibly abbreviated). Anything else, or an entry whose sha is not hex,
    is reported as a problem and left out rather than guessed at."""
    out = []
    entries = state.get("hand_resolved", [])
    if not isinstance(entries, list):
        problems.append(f"{os.path.basename(state_path)}: hand_resolved is not a list")
        return out
    for i, entry in enumerate(entries):
        sha = note = None
        if isinstance(entry, (list, tuple)) and len(entry) == 2:
            sha, note = entry
        elif isinstance(entry, dict):
            sha, note = entry.get("sha"), entry.get("note")
        elif isinstance(entry, str):
            parts = entry.split(None, 1)
            if len(parts) == 2:
                sha, note = parts
        if not (isinstance(sha, str) and re.fullmatch(r"[0-9a-f]{7,40}", sha)
                and isinstance(note, str)):
            problems.append(f"{os.path.basename(state_path)}: hand_resolved[{i}] is "
                            "not [sha, note], {\"sha\", \"note\"} or \"sha note\"")
            continue
        out.append((sha, note))
    return out


def cell(text):
    """Make text safe for one GitHub-markdown table cell."""
    return str(text).replace("\r", " ").replace("\n", " ").replace("|", "\\|")


def code(text):
    """A code span; commit subjects and function names keep their exact text."""
    text = str(text)
    if "`" in text:
        return text
    return "`" + text + "`"


def first_sentences(text, n=1, limit=320):
    parts = re.split(r"(?<=[.;])\s+", (text or "").strip())
    out = " ".join(parts[:n])
    return out if len(out) <= limit else out[:limit - 3].rstrip() + "..."


def unique(seq):
    seen, out = set(), []
    for x in seq:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def plural(n, word, words=None):
    return f"{n} {word if n == 1 else (words or word + 's')}"


def id_key(fid):
    m = ID_RE.match(fid)
    return (m.group(1), int(m.group(2)), m.group(3) == "V", int(m.group(4)))


def table(header, rows):
    out = ["| " + " | ".join(header) + " |",
           "| " + " | ".join("---" for _ in header) + " |"]
    for r in rows:
        out.append("| " + " | ".join(cell(c) for c in r) + " |")
    return "\n".join(out)


class Git:
    """Read-only git queries; every answer is None when git is unavailable."""

    def __init__(self, repo):
        self.repo = repo
        self._head_subjects = None

    def run(self, *args):
        try:
            r = subprocess.run(["git", "-C", self.repo, *args], capture_output=True,
                               text=True, errors="replace", check=False)
        except OSError:
            return None
        return r.stdout if r.returncode == 0 else None

    def lines_at(self, rev, path):
        out = self.run("show", f"{rev}:{path}")
        return None if out is None else out.count("\n")

    def head_subjects(self):
        if self._head_subjects is None:
            out = self.run("log", "--format=%s", "HEAD")
            self._head_subjects = set(out.splitlines()) if out is not None else set()
        return self._head_subjects

    def subject(self, sha):
        out = self.run("log", "-1", "--format=%s", sha)
        return out.strip() if out else None

    def branch_exists(self, name):
        return self.run("rev-parse", "--verify", "--quiet", "refs/heads/" + name) is not None

    def commits_with_subject(self, subject):
        out = self.run("log", "--all", "--format=%h", "--fixed-strings", "--grep", subject)
        if out is None:
            return None
        shas = []
        for sha in unique(out.split()):
            if self.subject(sha) == subject:
                shas.append(sha)
        return shas

    def sha_on(self, ref, subject):
        """The newest commit on ref whose subject is exactly subject."""
        out = self.run("log", "--format=%h%x09%s", "--fixed-strings", "--grep", subject, ref)
        for line in (out or "").splitlines():
            sha, _, subj = line.partition("\t")
            if subj == subject:
                return sha
        return None


# --- Loading and classification --------------------------------------------

def load(results, git):
    chunks = sorted(os.path.basename(p)[:-5] for p in glob.glob(os.path.join(results, "reviews", "*.json")))
    reviews = {c: load_json(os.path.join(results, "reviews", c + ".json")) for c in chunks}
    verify = {c: load_json(os.path.join(results, "verify", c + ".json")) for c in chunks}
    fixes = {c: load_json(os.path.join(results, "fixes", c + ".json")) for c in chunks
             if os.path.exists(os.path.join(results, "fixes", c + ".json"))}
    problems = []

    findings = collections.OrderedDict()
    for c in chunks:
        vmap = {v["id"]: v for v in verify[c]["verdicts"]}
        for x in reviews[c]["findings"]:
            v = vmap.get(x["id"])
            if v is None:
                problems.append(f"{x['id']}: no verifier verdict")
                continue
            findings[x["id"]] = dict(
                id=x["id"], chunk=c, origin="reviewer", category=x["category"],
                severity=v["severity"], reviewer_severity=x["severity"],
                file=x["file"], line=x.get("line"), function=x.get("function"),
                controller=x["input_controller"], description=x["description"],
                scenario=x.get("failure_scenario", ""), verdict=v["verdict"],
                evidence=v.get("evidence", ""), runtime_check=v.get("runtime_check", ""))
        for x in verify[c]["missed_findings"]:
            findings[x["id"]] = dict(
                id=x["id"], chunk=c, origin="verifier", category=x["category"],
                severity=x["severity"], reviewer_severity=None, file=x["file"],
                line=x.get("line"), function=x.get("function"),
                controller=x.get("input_controller", "internal-invariant"),
                description=x["description"], scenario=x.get("failure_scenario", ""),
                verdict="verifier-added", evidence="", runtime_check="")
        extra = set(vmap) - {x["id"] for x in reviews[c]["findings"]}
        if extra:
            problems.append(f"{c}: verdicts for unknown findings {sorted(extra)}")
    for f in findings.values():
        f.update(commits=[], skips=[], groups=collections.OrderedDict(), queued=None)
        if not ID_RE.match(f["id"]):
            problems.append(f"{f['id']}: unexpected ID format")

    commits = []
    observations = []
    for c, d in fixes.items():
        for k in d.get("commits", []):
            ids = [i for i in k.get("finding_ids", []) if ID_RE.match(i)]
            rec = dict(chunk=c, branch=d.get("branch"), sha=k["sha"], subject=k["subject"],
                       ids=ids, test=(k.get("test_added") or "").strip())
            commits.append(rec)
            for i in ids:
                if i in findings:
                    findings[i]["commits"].append(rec)
                else:
                    problems.append(f"{c} commit {k['sha']}: unknown finding {i}")
        for s in d.get("skipped", []):
            if ID_RE.match(s["id"]) and s["id"] in findings:
                findings[s["id"]]["skips"].append(s["reason"])
            elif s["id"].startswith(("oos:", "NEW:", "pre-existing:")):
                observations.append(dict(chunk=c, label=s["id"], reason=s["reason"]))

    groups = collections.OrderedDict()
    for path in sorted(glob.glob(os.path.join(results, "deferred", "*.json"))):
        if os.path.basename(path) == QUEUED_FILE:
            continue
        d = load_json(path)
        groups[d["group"]] = d
        for x in d["findings"]:
            if x["id"] in findings:
                findings[x["id"]]["groups"][d["group"]] = x["kind"]
            else:
                problems.append(f"group {d['group']}: unknown finding {x['id']}")
    queued_path = os.path.join(results, "deferred", QUEUED_FILE)
    queued = load_json(queued_path) if os.path.exists(queued_path) else []
    for q in queued:
        if q["id"] in findings:
            findings[q["id"]]["queued"] = q["kind"]
        else:
            problems.append(f"queued: unknown finding {q['id']}")

    for fid, other in SAME_DEFECT_AS.items():
        f = findings.get(fid)
        if f and not f["commits"] and other in findings:
            f["commits"] = list(findings[other]["commits"])
            f["same_as"] = other

    # Integration (split into several merge-plan*/integrate-state* pairs):
    # chunk commits an integrator skipped by hand, with its reason.
    superseded = {}
    for state_path in sorted(glob.glob(os.path.join(results, "integrate-state*.json"))):
        try:
            state = load_json(state_path)
        except ValueError as exc:
            print(f"warning: {state_path} unreadable ({exc}); its integration notes "
                  f"are left out", file=sys.stderr)
            continue
        for sha, note in hand_resolved_notes(state, state_path, problems):
            if "SKIPPED" in note:
                why = note[note.index("SKIPPED") + len("SKIPPED"):].lstrip(" ,:")
                superseded[sha[:7]] = re.sub(r"^as\s+", "", why)
    for f in findings.values():
        f["superseded"] = [superseded[c["sha"][:7]] for c in f["commits"]
                           if c["sha"][:7] in superseded]
        f["resolution"] = classify(f)
        if f["resolution"] is None:
            problems.append(f"{f['id']}: no resolution (no fix, skip, group or queue entry)")

    plan = []
    for plan_path in sorted(glob.glob(os.path.join(results, "merge-plan*.json"))):
        plan.extend(load_json(plan_path))

    infra_path = os.path.join(results, "fixes", "sanitizer-infra.json")
    infra = load_json(infra_path) if os.path.exists(infra_path) else {}

    dg_docs = parse_dg_docs(os.path.join(results, "deferred", DG_DOCS_FILE))

    coverage = {}
    for area, _, files in AREAS:
        area_chunks = [c for c in chunks if c[0] == area]
        for path in files:
            entries = [(c, e) for c in area_chunks for e in reviews[c]["files_read"]
                       if e["file"] == path]
            full = [(c, e) for c, e in entries if e["fully_read"]]
            pick = full[0] if full else (entries[0] if entries else (None, {}))
            coverage[path] = dict(area=area, chunk=pick[0], fully_read=bool(full),
                                  purpose=pick[1].get("purpose", ""),
                                  lines=git.lines_at(REVIEW_BASE, path))
            if pick[0] is None:
                problems.append(f"{path}: no reviewer listed it")
            if len(full) > 1:
                problems.append(f"{path}: fully read by several chunks {[c for c, _ in full]}")
    for f in findings.values():
        if f["file"] not in coverage:
            problems.append(f"{f['id']}: file {f['file']} is outside the audit scope")

    return dict(chunks=chunks, reviews=reviews, verify=verify, fixes=fixes,
                findings=findings, commits=commits, groups=groups, queued=queued,
                observations=observations, plan=plan, infra=infra, dg_docs=dg_docs,
                coverage=coverage, superseded=superseded, problems=problems, git=git)


def classify(f):
    if f["verdict"] == "refuted":
        return REFUTED
    has_fix = bool(f["commits"])
    if f["groups"]:
        if not has_fix:
            return DEFERRED
        # A follow-up group that lists a finding as "plausible, never
        # attempted" although a chunk branch already fixed it only has to
        # confirm that fix.
        if all(k.startswith("plausible") for k in f["groups"].values()):
            return FIXED
        return PART_DEFERRED
    if f["queued"]:
        return PART_QUEUED if has_fix else QUEUED
    if has_fix or f["id"] in FIXED_EARLIER:
        return FIXED
    if f["skips"]:
        return SKIPPED
    return None


def parse_dg_docs(path):
    """The numbered findings and the merge note in the DG docs agent's file."""
    if not os.path.exists(path):
        return dict(items=[], note=None, default_owner=None)
    items, note, default_owner = [], None, "dg follow-up"
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            m = re.match(r"^(\d+)\.\s+(.*)$", line)
            if m:
                text = m.group(2)
                owner = re.search(r"owner: ([^)]+)\)", text)
                sev = "high" if re.search(r"\bHIGH\b", text) else "not rated"
                text = re.sub(r"\s*\bHIGH\.?\s*$", "", text)
                items.append(dict(n=int(m.group(1)), text=text,
                                  owner=owner.group(1).strip() if owner else default_owner,
                                  severity=sev))
            elif line.startswith("Merge note:"):
                note = line[len("Merge note:"):].strip()
    return dict(items=items, note=note, default_owner=default_owner)


# --- Wording for one finding -----------------------------------------------

def where(f):
    tail = f["function"] if f.get("function") else (f.get("line") or "?")
    return code(f"{f['file']}:{tail}")


def sev_cell(f):
    if f["verdict"] == "refuted":
        return f"- (claimed {f['reviewer_severity']})"
    if f["reviewer_severity"] and f["reviewer_severity"] != f["severity"]:
        return f"{f['severity']} (was {f['reviewer_severity']})"
    return f["severity"]


def group_list(f):
    return ", ".join(code(g) for g in f["groups"])


def resolution_text(f, git):
    r = f["resolution"]
    subjects = unique(c["subject"] for c in f["commits"])
    fixes = "<br>".join(code(s) for s in subjects)
    if r == FIXED:
        if f["id"] in FIXED_EARLIER and not subjects:
            sha, subj = FIXED_EARLIER[f["id"]]
            text = f"fixed on the audit branch before the fix pass: {code(subj)} ({sha})"
        else:
            text = "fixed: " + fixes
        if f.get("same_as"):
            text += f"<br>(the same defect as {f['same_as']}; fixed by that chunk's commit)"
        if f["groups"]:
            text += f"<br>(follow-up {group_list(f)} asked to confirm the fix)"
    elif r == PART_DEFERRED:
        text = f"fixed in part: {fixes}<br>rest: follow-up {group_list(f)}"
    elif r == PART_QUEUED:
        text = f"fixed in part: {fixes}<br>rest: queued for a later round"
    elif r == DEFERRED:
        kinds = set(f["groups"].values())
        how = " (plausible, not attempted yet)" if all(k.startswith("plausible") for k in kinds) else ""
        text = f"deferred: follow-up {group_list(f)}{how}"
    elif r == QUEUED:
        text = "queued for a later round (not memory safety)"
    elif r == SKIPPED:
        text = "skipped: " + SKIP_REASON.get(f["id"], first_sentences(f["skips"][0]))
    elif r == REFUTED:
        text = "refuted: " + REFUTED_REASON.get(f["id"], first_sentences(f["evidence"]))
    else:
        text = "UNRESOLVED"
    if f["id"] in ALSO_FIXED_BY:
        other = ALSO_FIXED_BY[f["id"]]
        on_head = " (already on this branch)" if other in git.head_subjects() else ""
        text += f"<br>also fixed directly by {code(other)}{on_head}"
    for why in f["superseded"]:
        text += f"<br>integration skipped this commit: {why}"
    return text


def status_short(f):
    r = f["resolution"]
    if r == FIXED:
        if f["id"] in FIXED_EARLIER and not f["commits"]:
            return "fixed (on the audit branch)"
        return "fixed"
    if r == PART_DEFERRED:
        return "partly fixed; rest: " + ", ".join(f["groups"])
    if r == PART_QUEUED:
        return "partly fixed; rest queued"
    if r == DEFERRED:
        return "open: " + ", ".join(f["groups"])
    if r == QUEUED:
        return "open: queued"
    return r


def trigger_label(f):
    if f["id"] in TRIGGER_OVERRIDE:
        return TRIGGER_OVERRIDE[f["id"]]
    ctl = f["controller"]
    if ctl == "save-or-world-file":
        kind = "save file" if f["file"] in SAVE_FILE_SOURCES else "world file"
        if f["file"].startswith("src/util/"):
            kind += " (offline tool)"
        return kind
    return CONTROLLER_LABEL.get(ctl, ctl)


# --- Counting ----------------------------------------------------------------

def is_runtime(text):
    t = (text or "").lower()
    asan = bool(re.search(r"\basan\b|addresssanitizer|heap-use-after-free|"
                          r"heap-buffer-overflow|stack-buffer|global-buffer", t))
    ubsan = bool(re.search(r"ubsan|runtime error", t))
    vg = "valgrind" in t
    return asan, ubsan, vg


def compute(a):
    F = list(a["findings"].values())
    C = collections.Counter
    n = {}
    n["files"] = sum(len(files) for _, _, files in AREAS)
    n["areas"] = len(AREAS)
    n["chunks"] = len(a["chunks"])
    lines = [c["lines"] for c in a["coverage"].values()]
    n["lines"] = sum(lines) if all(x is not None for x in lines) else None
    n["fully_read"] = sum(1 for c in a["coverage"].values() if c["fully_read"])
    n["not_fully_read"] = sorted(p for p, c in a["coverage"].items() if not c["fully_read"])
    n["findings"] = len(F)
    n["reviewer_findings"] = sum(1 for f in F if f["origin"] == "reviewer")
    n["verifier_added"] = sum(1 for f in F if f["origin"] == "verifier")
    n["verdict"] = C(f["verdict"] for f in F)
    n["severity_real"] = C(f["severity"] for f in F if f["verdict"] != "refuted")
    n["severity_reviewer"] = C(f["reviewer_severity"] for f in F if f["origin"] == "reviewer")
    n["refuted_claimed"] = C(f["reviewer_severity"] for f in F if f["verdict"] == "refuted")
    n["resolution"] = C(f["resolution"] for f in F)
    n["category"] = C(f["category"] for f in F)
    n["controller"] = C(f["controller"] for f in F)
    n["open"] = sum(1 for f in F if f["resolution"] in OPEN)
    n["severity_changed"] = sum(1 for f in F if f["origin"] == "reviewer"
                                and f["verdict"] != "refuted"
                                and f["severity"] != f["reviewer_severity"])
    # Verifier runtime evidence (reviewer findings only: added ones have no verdict).
    rt = C()
    for c in a["chunks"]:
        for v in a["verify"][c]["verdicts"]:
            asan, ubsan, vg = is_runtime(v["runtime_check"])
            rt["verdicts"] += 1
            rt["asan"] += asan
            rt["ubsan"] += ubsan
            rt["valgrind"] += vg
            any_run = asan or ubsan or vg
            rt["any"] += any_run
            rt[("any", v["severity"])] += any_run
            rt[("all", v["severity"])] += 1
    n["runtime"] = rt
    # Fix commits.
    kinds = C()
    for k in a["commits"]:
        kinds[k["subject"].split(":", 1)[0] if ":" in k["subject"] else "other"] += 1
    n["commits"] = len(a["commits"])
    n["commit_kinds"] = kinds
    n["commits_with_ids"] = sum(1 for k in a["commits"] if k["ids"])
    n["commits_with_test"] = sum(1 for k in a["commits"]
                                 if k["test"] and not k["test"].lower().startswith("none"))
    n["fix_branches"] = len(a["fixes"])
    n["doc_gaps"] = sum(len(a["reviews"][c]["doc_gaps"]) for c in a["chunks"])
    n["macro_proposals"] = sum(len(a["reviews"][c]["macro_proposals"]) for c in a["chunks"])
    n["deferred_macros"] = sum(len(d.get("deferred_macros", [])) for d in a["fixes"].values())
    # Follow-up groups.
    g_ids = collections.OrderedDict()
    for g, d in a["groups"].items():
        g_ids[g] = unique(x["id"] for x in d["findings"])
    n["groups"] = len(a["groups"])
    n["group_entries"] = sum(len(d["findings"]) for d in a["groups"].values())
    n["group_unique"] = len(set().union(*g_ids.values())) if g_ids else 0
    kind_first = {}
    for g, d in a["groups"].items():
        for x in d["findings"]:
            kind_first.setdefault(x["id"], x["kind"])
    n["group_kinds"] = C(kind_first.values())
    n["queued"] = len(a["queued"])
    n["queued_kinds"] = C(q["kind"] for q in a["queued"])
    n["plausible"] = sum(1 for f in F if f["verdict"] == "plausible")
    n["plausible_fixed"] = sum(1 for f in F if f["verdict"] == "plausible" and f["resolution"] == FIXED)
    n["plausible_unfixed"] = [f["id"] for f in F if f["verdict"] == "plausible" and f["resolution"] != FIXED]
    n["plausible_unscheduled"] = [f["id"] for f in F if f["verdict"] == "plausible"
                                  and f["resolution"] not in (FIXED, DEFERRED, QUEUED,
                                                              PART_DEFERRED, PART_QUEUED)]
    n["dg_docs"] = len(a["dg_docs"]["items"])
    n["plan_branches"] = len(a["plan"])
    n["plan_commits"] = sum(len(p["commits"]) for p in a["plan"])
    n["observations"] = len(a["observations"])
    n["problems"] = list(a["problems"])
    git = a["git"]
    shas = git.commits_with_subject(HOTFIX_SUBJECT)
    n["hotfix_backports"] = len(shas) if shas is not None else None
    n["hotfix_master"] = git.sha_on("master", HOTFIX_SUBJECT)
    return n


def fmt_int(x):
    return "unknown" if x is None else f"{x:,}"


# --- Generated sections: the ledger -----------------------------------------

def gen_ledger_numbers(a, n):
    rt = n["runtime"]
    kinds = n["commit_kinds"]
    kind_text = ", ".join(f"{kinds[k]} {k}" for k in
                          ["security", "diag", "fix", "docs", "style", "test"] if kinds[k])
    not_full = ", ".join(code(p) for p in n["not_fully_read"]) or "none"
    lines = [
        f"- Scope: {n['files']} files in {n['areas']} areas, "
        f"{fmt_int(n['lines'])} lines at the review base {code(REVIEW_BASE)}, "
        f"split into {n['chunks']} chunks.",
        f"- Coverage: {n['fully_read']} of {n['files']} files were read line by line "
        f"by the reviewer of the chunk that owns them. Not read in full: {not_full}.",
        f"- Findings: {n['findings']} ({n['reviewer_findings']} from reviewers, "
        f"{n['verifier_added']} added by verifiers).",
        f"- Verdicts: {n['verdict']['confirmed']} confirmed, "
        f"{n['verdict']['plausible']} plausible, {n['verdict']['refuted']} refuted; "
        f"{n['severity_changed']} severities re-rated by the verifiers.",
        f"- Runtime evidence: {rt['any']} of {rt['verdicts']} verdicts cite an ASan, "
        f"UBSan or Valgrind run (Valgrind {rt['valgrind']}, ASan {rt['asan']}, "
        f"UBSan {rt['ubsan']}), including {rt[('any', 'critical')]} of "
        f"{rt[('all', 'critical')]} critical and {rt[('any', 'high')]} of "
        f"{rt[('all', 'high')]} high verdicts.",
        f"- Fix commits: {n['commits']} on {n['fix_branches']} chunk branches "
        f"({kind_text}); {n['commits_with_ids']} name finding IDs and "
        f"{n['commits_with_test']} record a regression test.",
        f"- Documentation and macros: reviewers reported {n['doc_gaps']} documentation "
        f"gaps and {n['macro_proposals']} macro proposals; fix agents passed "
        f"{n['deferred_macros']} cross-cutting macros on to the core-utils follow-up.",
        f"- Follow-up: {n['group_unique']} findings in {n['groups']} follow-up groups, "
        f"{n['queued']} queued for a later round, and "
        f"{n['dg_docs']} new DG findings from the documentation pass.",
        f"- Integration: the merge plans cherry-pick {n['plan_commits']} commits from "
        f"{n['plan_branches']} branches onto {code('audit/full-hardening-integration')} "
        f"and {code('audit/full-hardening-integration-b')}.",
    ]
    return "\n".join(lines)


def file_status(path, cov, flist):
    if cov["chunk"] is None:
        return "not reviewed"
    base = "reviewed" if cov["fully_read"] else "reviewed (not every line, see note)"
    n_open = sum(1 for f in flist if f["resolution"] in OPEN)
    branch = any(f["resolution"] in HAS_BRANCH_FIX and f["commits"] for f in flist)
    if not branch and not n_open:
        return base + "; done"
    parts = [base]
    if branch:
        parts.append("fixes on branch")
    if n_open:
        parts.append(f"{n_open} open")
    return "; ".join(parts)


def findings_cell(flist):
    if not flist:
        return "none"
    r = collections.Counter(f["resolution"] for f in flist)
    bits = []
    for label, keys in (("fixed", [FIXED]), ("partly fixed", [PART_DEFERRED, PART_QUEUED]),
                        ("open", [DEFERRED, QUEUED]), ("skipped", [SKIPPED]),
                        ("refuted", [REFUTED])):
        k = sum(r[x] for x in keys)
        if k:
            bits.append(f"{k} {label}")
    return f"{len(flist)}: " + ", ".join(bits)


def by_file(a):
    out = collections.defaultdict(list)
    for f in a["findings"].values():
        out[f["file"]].append(f)
    return out


def gen_ledger_summary(a, n):
    files_f = by_file(a)
    rows = []
    tot = collections.Counter()
    for area, name, files in AREAS:
        fl = [f for p in files for f in files_f[p]]
        r = collections.Counter(f["resolution"] for f in fl)
        row = dict(files=len(files),
                   full=sum(1 for p in files if a["coverage"][p]["fully_read"]),
                   findings=len(fl), fixed=r[FIXED],
                   partly=r[PART_DEFERRED] + r[PART_QUEUED],
                   open=r[DEFERRED] + r[QUEUED], skipped=r[SKIPPED], refuted=r[REFUTED])
        tot.update(row)
        rows.append([f"{area}: {name}", row["files"], row["full"], row["findings"],
                     row["fixed"], row["partly"], row["open"], row["skipped"], row["refuted"]])
    rows.append(["**Total**", tot["files"], tot["full"], tot["findings"], tot["fixed"],
                 tot["partly"], tot["open"], tot["skipped"], tot["refuted"]])
    head = ["Area", "Files", "Read in full", "Findings", "Fixed", "Partly fixed",
            "Open", "Skipped", "Refuted"]
    note = ("Findings are counted against the file they name, so a finding one "
            "chunk reported about another area's file counts in that file's area. "
            "\"Partly fixed\" means a chunk branch fixed the part in its own files "
            "and the rest is in a follow-up group or queued; \"open\" means no fix "
            "yet.")
    return table(head, rows) + "\n\n" + note


def gen_ledger_areas(a, n):
    files_f = by_file(a)
    out = []
    for area, name, files in AREAS:
        chunks = [c for c in a["chunks"] if c[0] == area]
        fl = [f for p in files for f in files_f[p]]
        statuses = {p: file_status(p, a["coverage"][p], files_f[p]) for p in files}
        not_full = [p for p in files if not a["coverage"][p]["fully_read"]]
        n_done = sum(1 for s in statuses.values() if s.endswith("done"))
        n_branch = sum(1 for s in statuses.values() if "fixes on branch" in s)
        n_open = sum(1 for f in fl if f["resolution"] in OPEN)
        read = ("every file read in full" if not not_full else
                "every file read in full except " + ", ".join(code(p) for p in not_full))
        out.append(f"### Area {area}: {name}\n")
        out.append(f"Status: reviewed by {', '.join(chunks)} ({read}); "
                   f"{plural(n_done, 'file')} done, {n_branch} with fixes on branch; "
                   f"{plural(n_open, 'finding')} against these files still open.\n")
        rows = []
        for c in chunks:
            owned = [p for p in files if a["coverage"][p]["chunk"] == c]
            ln = [a["coverage"][p]["lines"] for p in owned]
            fx = a["fixes"].get(c, {})
            nrev = len(a["reviews"][c]["findings"])
            nadd = len(a["verify"][c]["missed_findings"])
            rows.append([c, ", ".join(p[4:] for p in owned),
                         fmt_int(sum(ln)) if all(x is not None for x in ln) else "unknown",
                         f"{nrev} + {nadd} verifier-added" if nadd else str(nrev),
                         code(fx.get("branch", "none")), len(fx.get("commits", []))])
        out.append(table(["Chunk", "Files", "Lines", "Findings reported",
                          "Fix branch", "Fix commits"], rows) + "\n")
        rows = [[code(p), statuses[p], findings_cell(files_f[p])] for p in files]
        out.append(table(["File", "Status", "Findings"], rows))
        for p in not_full:
            cov = a["coverage"][p]
            out.append(f"\nNote: the {cov['chunk']} reviewer did not read every line of "
                       f"{code(p)}: \"{cell(cov['purpose'])}\"")
        out.append("")
    return "\n".join(out).rstrip()


def gen_ledger_findings(a, n):
    git = a["git"]
    out = []
    for area, name, _ in AREAS:
        fl = sorted((f for f in a["findings"].values() if f["id"][0] == area),
                    key=lambda f: id_key(f["id"]))
        r = collections.Counter(f["resolution"] for f in fl)
        summary = "; ".join(f"{r[k]} {k}" for k in RESOLUTIONS if r[k])
        out.append(f"### Area {area}: {name}\n")
        out.append(f"{plural(len(fl), 'finding')} from chunks "
                   f"{', '.join(c for c in a['chunks'] if c[0] == area)}: {summary}.\n")
        rows = [[f["id"], sev_cell(f), f["category"], where(f), f["controller"],
                 f["verdict"], resolution_text(f, git)] for f in fl]
        out.append(table(["ID", "Severity", "Category", "File:function",
                          "Input controller", "Verdict", "Resolution"], rows))
        out.append("")
    return "\n".join(out).rstrip()


def gen_extra_fixes(a, n):
    git = a["git"]
    head = git.head_subjects()
    rows = []
    for x in EXTRA_FIXES:
        on_head = [s for s in x["subjects"] if s in head]
        where_txt = "this branch" if on_head else x["branch"]
        if on_head and len(x["subjects"]) > 1:
            where_txt = f"{x['branch']}; this branch has {code(on_head[0])}"
        fix = "<br>".join(code(s) for s in x["subjects"])
        notes = x.get("notes", "")
        if x.get("backport") and n["hotfix_backports"]:
            notes = (notes + "; " if notes else "") + (
                f"backported as {code(HOTFIX_SUBJECT)} to {n['hotfix_backports']} "
                f"local branches (master: {n['hotfix_master'] or 'unknown'})")
        rows.append([x["bug"], x["found"], x["trigger"], fix, where_txt,
                     x["related"] or "-", notes or "-"])
    return table(["Bug", "Found by", "Who or what triggers it", "Fix (commit subject)",
                  "Where the fix is", "Chunk finding", "Notes"], rows)


def gen_dg_docs(a, n):
    d = a["dg_docs"]
    if not d["items"]:
        return "No new findings file was found."
    rows = [[f"DGDOC-{x['n']}", x["text"], x["severity"], x["owner"], "open"]
            for x in d["items"]]
    out = table(["Label", "Finding (as reported)", "Severity", "Fix owner", "Status"], rows)
    if d["note"]:
        out += "\n\nMerge note from the same file: " + cell(d["note"])
    return out


def gen_groups(a, n):
    git = a["git"]
    rows = []
    for g, d in a["groups"].items():
        ids = unique(x["id"] for x in d["findings"])
        r = collections.Counter(a["findings"][i]["resolution"] for i in ids)
        branch = f"audit-deferred-{g}"
        rows.append([code(g), code(branch) if git.branch_exists(branch) else "-",
                     code(d.get("base_branch", "")), len(d.get("owned_files", [])), len(ids),
                     r[PART_DEFERRED], r[DEFERRED], r[FIXED], r[REFUTED]])
    return table(["Group", "Branch", "Based on", "Files owned", "Findings",
                  "Partly fixed", "Not started", "Fixed on a chunk branch (confirm)",
                  "Refuted"], rows)


def gen_ledger_outside(a, n):
    parts = ["### Found outside the chunk review\n",
             "Fixed directly by the coordinator. A related chunk finding means a "
             "chunk reviewer reported the same bug and its fix agent fixed it on "
             "the chunk branch too; integration keeps one of the two.\n",
             gen_extra_fixes(a, n),
             "\n### New DG findings from the documentation pass (open)\n",
             "Reported by the dg-scripts documentation agent after the fix pass; "
             "none is covered by a fix branch yet. They have no chunk ID; the "
             "DGDOC labels are this ledger's.\n",
             gen_dg_docs(a, n),
             "\n### Follow-up groups\n",
             "Findings whose fix needs files outside the reporting chunk, the "
             "plausible low and medium findings no fix agent was asked to fix, and "
             "the error-locality findings, regrouped by the files they touch. A "
             "finding can be in two groups when each owns half of the fix.\n",
             gen_groups(a, n)]
    return "\n".join(parts)


# --- Generated sections: the report -----------------------------------------

def sev_row(counter, keys=SEVERITIES):
    return [counter[k] for k in keys]


def gen_report_glance(a, n):
    r = n["resolution"]
    sv = n["severity_real"]
    lines = [
        f"- **Scope:** {n['files']} source files, {fmt_int(n['lines'])} lines at the "
        f"review base {code(REVIEW_BASE)}, in {n['areas']} areas and {n['chunks']} "
        f"review chunks. {n['fully_read']} files were read line by line; "
        f"{', '.join(code(p) for p in n['not_fully_read']) or 'none'} was not "
        f"(see the ledger note).",
        f"- **Findings:** {n['findings']} ({n['reviewer_findings']} from the reviewers, "
        f"{n['verifier_added']} added by the verifiers). After verification: "
        f"{n['verdict']['confirmed']} confirmed, {n['verdict']['plausible']} plausible, "
        f"{n['verdict']['refuted']} refuted.",
        f"- **Severity** of the {n['findings'] - n['verdict']['refuted']} findings that "
        f"stand: {sv['critical']} critical, {sv['high']} high, {sv['medium']} medium, "
        f"{sv['low']} low.",
        f"- **Resolution:** {r[FIXED]} fixed, {r[PART_DEFERRED] + r[PART_QUEUED]} "
        f"partly fixed (the rest handed on), {r[DEFERRED]} deferred to follow-up "
        f"groups, {r[QUEUED]} queued for a later round, {r[SKIPPED]} skipped (no "
        f"change needed), {r[REFUTED]} refuted.",
        f"- **Commits:** {n['commits']} fix commits on {n['fix_branches']} chunk "
        f"branches, {n['commits_with_test']} of them with a regression test; plus "
        f"the infrastructure and coordinator branches listed below.",
        f"- **Still open:** {n['open']} findings (partly fixed or not started), "
        f"{n['dg_docs']} new DG findings from the documentation pass, and the "
        f"integration of {n['plan_branches']} branches.",
    ]
    return "\n".join(lines)


def gen_report_counts(a, n):
    F = list(a["findings"].values())
    out = []
    # Verdict x severity.
    rows = []
    for v in VERDICTS:
        fl = [f for f in F if f["verdict"] == v]
        if v == "refuted":
            c = collections.Counter(f["reviewer_severity"] for f in fl)
            rows.append([f"{v} (severity the reviewer claimed)", len(fl), *sev_row(c)])
        else:
            c = collections.Counter(f["severity"] for f in fl)
            rows.append([v, len(fl), *sev_row(c)])
    out.append(f"**By verdict and severity** ({len(F)} findings; severity as re-rated "
               f"by the verifier):\n")
    out.append(table(["Verdict", "Findings", "Critical", "High", "Medium", "Low"], rows))
    # Severity x resolution.
    cols = [FIXED, PART_DEFERRED, PART_QUEUED, DEFERRED, QUEUED, SKIPPED]
    rows = []
    for s in SEVERITIES:
        fl = [f for f in F if f["verdict"] != "refuted" and f["severity"] == s]
        c = collections.Counter(f["resolution"] for f in fl)
        rows.append([s, len(fl), *[c[k] for k in cols]])
    fl = [f for f in F if f["verdict"] != "refuted"]
    c = collections.Counter(f["resolution"] for f in fl)
    rows.append(["**Total**", len(fl), *[c[k] for k in cols]])
    out.append("\n**By severity and resolution** (refuted findings left out):\n")
    out.append(table(["Severity", "Findings", "Fixed", "Partly fixed, rest in a group",
                      "Partly fixed, rest queued", "Deferred to a group",
                      "Queued", "Skipped"], rows))
    # Category.
    rows = []
    for cat in CATEGORIES:
        fl = [f for f in F if f["category"] == cat]
        if not fl:
            continue
        real = [f for f in fl if f["verdict"] != "refuted"]
        c = collections.Counter(f["severity"] for f in real)
        rows.append([cat, len(fl), *sev_row(c), len(fl) - len(real),
                     sum(1 for f in fl if f["resolution"] == FIXED)])
    out.append("\n**By category:**\n")
    out.append(table(["Category", "Findings", "Critical", "High", "Medium", "Low",
                      "Refuted", "Fixed"], rows))
    # Controller.
    rows = []
    for ctl in CONTROLLERS:
        fl = [f for f in F if f["controller"] == ctl]
        real = [f for f in fl if f["verdict"] != "refuted"]
        c = collections.Counter(f["severity"] for f in real)
        rows.append([ctl, CONTROLLER_LABEL[ctl], len(fl), *sev_row(c),
                     len(fl) - len(real)])
    out.append("\n**By who controls the triggering input:**\n")
    out.append(table(["Input controller", "Meaning", "Findings", "Critical", "High",
                      "Medium", "Low", "Refuted"], rows))
    # Commits.
    k = n["commit_kinds"]
    rows = [[kind, k[kind]] for kind in sorted(k, key=lambda x: -k[x])]
    rows.append(["**Total**", n["commits"]])
    out.append(f"\n**Fix commits on the chunk branches** ({n['commits_with_ids']} name "
               f"finding IDs; {n['commits_with_test']} record a regression test):\n")
    out.append(table(["Kind (subject prefix)", "Commits"], rows))
    # Runtime evidence.
    rt = n["runtime"]
    rows = [[s, rt[("all", s)], rt[("any", s)], rt[("all", s)] - rt[("any", s)]]
            for s in SEVERITIES]
    rows.append(["**Total**", rt["verdicts"], rt["any"], rt["verdicts"] - rt["any"]])
    out.append("\n**Verifier runtime evidence** (reviewer findings; verifier severity):\n")
    out.append(table(["Severity", "Verdicts", "ASan, UBSan or Valgrind run",
                      "No sanitizer or Valgrind run"], rows))
    return "\n".join(out)


EXPOSURE = {c: i for i, c in enumerate(CONTROLLERS)}


def serious_table(a, severity):
    fl = [f for f in a["findings"].values()
          if f["severity"] == severity and f["resolution"] not in (REFUTED, SKIPPED)]
    fl.sort(key=lambda f: (EXPOSURE.get(f["controller"], 99), id_key(f["id"])))
    rows = [[f["id"], where(f), trigger_label(f),
             IMPACT.get(f["id"], first_sentences(f["scenario"], 1, 200)),
             status_short(f)] for f in fl]
    return table(["ID", "Where", "Who can trigger it", "Impact", "Status"], rows)


def gen_report_critical(a, n):
    return serious_table(a, "critical")


def gen_report_high(a, n):
    skipped = [f["id"] for f in a["findings"].values()
               if f["severity"] == "high" and f["resolution"] == SKIPPED]
    out = serious_table(a, "high")
    if skipped:
        out += ("\n\nLeft out: " + ", ".join(skipped) +
                " (rated high, but the fix agent found the code already safe).")
    return out


def gen_report_extra(a, n):
    return gen_extra_fixes(a, n)


def done_later(proposal):
    for prefix, subjects in INFRA_DONE_LATER.items():
        if proposal["title"].startswith(prefix):
            return subjects
    return None


def gen_report_infra(a, n):
    git = a["git"]
    rows = []
    for p in a["infra"].get("proposals", []):
        impl = p.get("implemented", p.get("tested", ""))
        if impl and not impl.startswith("not implemented"):
            subs = []
            for sha in re.findall(r"[0-9a-f]{7,40}", impl):
                s = git.subject(sha)
                subs.append(code(s) if s else sha)
            impl = "<br>".join(subs)
        elif done_later(p):
            impl = (impl + "; done afterwards by the coordinator: " +
                    "<br>".join(code(s) for s in done_later(p)))
        rows.append([p["title"], p["priority"], impl])
    return table(["Proposal", "Priority", "Commit (subject) or status"], rows)


def gen_report_open(a, n):
    F = a["findings"]
    out = []
    out.append("**Follow-up groups** (fixes in progress on the branches shown):\n")
    out.append(gen_groups(a, n))
    # Open findings by group.
    rows = []
    for g, d in a["groups"].items():
        ids = unique(x["id"] for x in d["findings"])
        part = [i for i in ids if F[i]["resolution"] == PART_DEFERRED]
        todo = [i for i in ids if F[i]["resolution"] == DEFERRED]
        rows.append([code(g), ", ".join(part) or "-", ", ".join(todo) or "-"])
    out.append("\n**Open findings per group:**\n")
    out.append(table(["Group", "Partly fixed (rest open)", "Not started"], rows))
    # Queued.
    rows = []
    for q in a["queued"]:
        f = F[q["id"]]
        rows.append([q["id"], code(f["file"]), f["category"], f["severity"], q["kind"],
                     status_short(f)])
    out.append(f"\n**Queued for a later round** ({len(a['queued'])} findings, not "
               f"memory safety, in files the follow-up groups own):\n")
    out.append(table(["ID", "File", "Category", "Severity", "Kind", "Status"], rows))
    # Plausible.
    unfixed = n["plausible_unfixed"]
    sched = collections.defaultdict(list)
    for i in unfixed:
        f = F[i]
        key = ", ".join(f["groups"]) if f["groups"] else ("queued" if f["queued"] else "nowhere")
        sched[key].append(i)
    rows = [[k, len(v), ", ".join(sorted(v, key=id_key))] for k, v in sorted(sched.items())]
    unsched = n["plausible_unscheduled"]
    where_txt = ("every one of them is scheduled in a follow-up group or the queue"
                 if not unsched else
                 f"{len(unsched)} are not scheduled anywhere: {', '.join(unsched)}")
    out.append(f"\n**Plausible findings not fixed yet:** {len(unfixed)} of "
               f"{n['plausible']} ({n['plausible_fixed']} were fixed); {where_txt}.\n")
    out.append(table(["Scheduled in", "Findings", "IDs"], rows))
    # Observations.
    if a["observations"]:
        rows = [[o["chunk"], o["label"], first_sentences(o["reason"], 5, 560)]
                for o in a["observations"]]
        out.append("\n**Observations the fix agents recorded but nobody has triaged** "
                   "(not findings; no group owns them):\n")
        out.append(table(["Chunk", "Observation", "What the fix agent wrote"], rows))
    # Infrastructure not implemented (and not done later by the coordinator).
    rows = [[p["title"], p["priority"], p.get("implemented", "")[len("not implemented: "):]]
            for p in a["infra"].get("proposals", [])
            if str(p.get("implemented", "")).startswith("not implemented")
            and not done_later(p)]
    if rows:
        out.append("\n**Infrastructure proposals still not implemented:**\n")
        out.append(table(["Proposal", "Priority", "Reason the infrastructure pass gave"],
                         rows))
    return "\n".join(out)


def gen_report_dg_docs(a, n):
    return gen_dg_docs(a, n)


GENERATORS = {
    LEDGER: {
        "ledger-numbers": gen_ledger_numbers,
        "ledger-summary": gen_ledger_summary,
        "ledger-areas": gen_ledger_areas,
        "ledger-findings": gen_ledger_findings,
        "ledger-outside": gen_ledger_outside,
    },
    REPORT: {
        "report-glance": gen_report_glance,
        "report-counts": gen_report_counts,
        "report-critical": gen_report_critical,
        "report-high": gen_report_high,
        "report-extra": gen_report_extra,
        "report-infra": gen_report_infra,
        "report-dg-docs": gen_report_dg_docs,
        "report-open": gen_report_open,
    },
}

MARK_RE = re.compile(r"(<!-- BEGIN GENERATED: ([a-z0-9-]+) -->\n)(.*?)(<!-- END GENERATED: \2 -->)",
                     re.S)


def render(path, gens, a, n):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    seen = set()

    def repl(m):
        name = m.group(2)
        if name not in gens:
            raise SystemExit(f"{path}: unknown generated section {name!r}")
        seen.add(name)
        # Blank lines keep the markers from gluing onto a table or a list.
        return m.group(1) + "\n" + gens[name](a, n).strip() + "\n\n" + m.group(4)

    new = MARK_RE.sub(repl, text)
    missing = set(gens) - seen
    if missing:
        raise SystemExit(f"{path}: missing generated sections {sorted(missing)}")
    return text, new


def headline(n):
    r = n["resolution"]
    v = n["verdict"]
    s = n["severity_real"]
    print(f"scope: {n['files']} files, {fmt_int(n['lines'])} lines at {REVIEW_BASE}, "
          f"{n['chunks']} chunks; read in full: {n['fully_read']} "
          f"(not: {', '.join(n['not_fully_read']) or 'none'})")
    print(f"findings: {n['findings']} = {n['reviewer_findings']} reviewer + "
          f"{n['verifier_added']} verifier-added")
    print("verdicts: " + ", ".join(f"{k} {v[k]}" for k in VERDICTS))
    print("severity (standing findings): " + ", ".join(f"{k} {s[k]}" for k in SEVERITIES))
    print("resolution: " + ", ".join(f"{k}: {r[k]}" for k in RESOLUTIONS))
    print(f"open: {n['open']}; follow-up groups: {n['groups']} with {n['group_unique']} "
          f"findings; queued: {n['queued']}; new DG doc findings: {n['dg_docs']}")
    print(f"commits: {n['commits']} on {n['fix_branches']} chunk branches; "
          f"with tests: {n['commits_with_test']}")
    if n["problems"]:
        print("PROBLEMS:")
        for p in n["problems"]:
            print("  " + p)


def jsonable(n):
    out = {}
    for k, v in n.items():
        if isinstance(v, collections.Counter):
            out[k] = {(" / ".join(x) if isinstance(x, tuple) else str(x)): c for x, c in v.items()}
        else:
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--results", default=DEFAULT_RESULTS, help="audit results directory")
    ap.add_argument("--repo", default=".", help="repository root (read-only git)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="rewrite the generated sections")
    mode.add_argument("--check", action="store_true", help="exit 1 if a section is stale")
    mode.add_argument("--json", action="store_true", help="print every count as JSON")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.results, "reviews")):
        sys.exit(f"{args.results}: no reviews/ directory (use --results)")
    git = Git(args.repo)
    a = load(args.results, git)
    n = compute(a)
    if args.json:
        json.dump(jsonable(n), sys.stdout, indent=1, sort_keys=True)
        print()
        return 0
    if not (args.write or args.check):
        headline(n)
        return 1 if n["problems"] else 0
    stale = []
    for rel, gens in GENERATORS.items():
        path = os.path.join(args.repo, rel)
        if not os.path.exists(path):
            print(f"{rel}: not found, skipped", file=sys.stderr)
            continue
        old, new = render(path, gens, a, n)
        if old != new:
            stale.append(rel)
            if args.write:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new)
    if n["problems"]:
        print("problems in the input:", *n["problems"], sep="\n  ", file=sys.stderr)
    if args.check:
        if stale:
            print("stale: " + ", ".join(stale))
            return 1
        print("up to date")
        return 0
    print("updated: " + (", ".join(stale) if stale else "nothing (already up to date)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
