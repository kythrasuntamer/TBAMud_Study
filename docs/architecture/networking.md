# Networking, login and the command interpreter

This document follows a byte from the network to a game command and back:
sockets and descriptors (`comm.c`), telnet negotiation and colour
(`protocol.c`, `telnet.h`), the login state machine and the command
interpreter (`interpreter.c`), site bans (`ban.c`), and the string editor
and pager (`modify.c`, `improved-edit.c`). Read
[overview.md](overview.md) first for the game loop and the data model.

## Contents

1. [Who controls each input](#who-controls-each-input)
2. [Source files](#source-files)
3. [Accepting a connection](#accepting-a-connection)
4. [Descriptor states and lifecycle](#descriptor-states-and-lifecycle)
5. [Input path](#input-path)
6. [Output path](#output-path)
7. [Closing a connection](#closing-a-connection)
8. [Telnet protocols, MSDP and colour](#telnet-protocols-msdp-and-colour)
9. [The login state machine](#the-login-state-machine)
10. [The command interpreter](#the-command-interpreter)
11. [Argument parsing helpers](#argument-parsing-helpers)
12. [Aliases](#aliases)
13. [Site bans and name rules](#site-bans-and-name-rules)
14. [The string editor](#the-string-editor)
15. [The pager](#the-pager)
16. [Open problems](#open-problems)

## Who controls each input

The server has one trust boundary that matters most: everything a socket
sends is untrusted until a password has been verified, and a logged-in
player's text is still untrusted *data* even though the player's identity is
known. Treat each input according to who can set it.

| Input | Who controls it | Where it enters | Notes |
| --- | --- | --- | --- |
| TCP connections | anyone who can reach the port | `comm.c:new_descriptor` | rate-limited per IPv4 address before anything is allocated |
| Raw bytes, telnet commands and subnegotiations | the remote client, before login | `comm.c:process_input`, `protocol.c:ProtocolInput` | bounded by `MAX_RAW_INPUT_LENGTH` (12 KB) per descriptor |
| Client identity (TTYPE, MSDP `CLIENT_ID`, MXP `CLIENT=`/`VERSION=`) | the client program | `protocol.c:PerformSubnegotiation`, `ExecuteMSDPPair` | arbitrary text; echoed back in the protocol banner |
| Reverse DNS name | whoever controls the PTR record | `gethostbyaddr()` in `new_descriptor` | truncated to `HOST_LENGTH` (40); used for bans and logs |
| Name, password, menu choices | the unauthenticated remote user | `interpreter.c:nanny` | names are letters only, 2 to 20 characters |
| Command lines | a logged-in player | `interpreter.c:command_interpreter` | printable ASCII only, at most 511 bytes |
| Editor text (mail, boards, descriptions, ideas/bugs/typos) | a logged-in player | `modify.c:string_add` | written to disk; `~` at a line end is removed |
| Commands with `minimum_level >= LVL_IMMORT` | immortals | `cmd_info[]` levels | identity trusted, arguments still validated |
| OLC input | builders (`LVL_BUILDER` and zone permissions) | `nanny()` -> each editor's `*_parse` | ends up in world files; see [olc.md](olc.md) |
| World, help and text files | builders and administrators, on disk | `db.c` loaders | validated at boot; see [persistence.md](persistence.md) |
| Script text and `%var%` values | builders, and players through variables | `dg_*.c` | see [dg-scripts.md](dg-scripts.md) |

Two rules follow everywhere in this layer:

- **Untrusted text is never a format string.** Use
  `write_to_output(d, "%s", text)`. The CMake build compiles with
  `-Werror=format-nonliteral -Werror=format-security`, and every output
  function carries a `format(printf)` attribute, so a non-literal format is
  a build error.
- **Never log raw player text.** It can contain control characters that
  split or forge log records. Log sizes and IDs, or an escaped excerpt (see
  `docs/maintainer-safety-guide.md`).

## Source files

| File | Role |
| --- | --- |
| `comm.c` / `comm.h` | `main()`, the game loop, sockets, input framing, output buffering, `close_socket()`, signal handlers, the `send_to_*` family, `act()`, logging setup, MSDP updates |
| `protocol.c` / `protocol.h` | KaVir's protocol snippet: telnet negotiation, MSDP/MSSP/MXP/ATCP/MSP, colour and unicode expansion, copyover state |
| `telnet.h` | the BSD `arpa/telnet.h` constants, used when the system header is missing |
| `interpreter.c` / `interpreter.h` | the command table, `command_interpreter()`, `nanny()`, aliases, argument parsing, password hashing, login rate limits |
| `ban.c` / `ban.h` | site bans and the invalid-name list |
| `modify.c` / `modify.h` | the string editor core (`string_write`, `string_add`), the pager, `smash_tilde`, `parse_at`/`parse_tab`, `do_skillset` |
| `improved-edit.c` / `improved-edit.h` | the `/`-command line editor and the text formatter |

## Accepting a connection

`comm.c:init_socket` creates the listening ("mother") socket: `SO_REUSEADDR`,
a send buffer of `MAX_SOCK_BUF` (24 KB), no linger, bound to `CONFIG_DFLT_IP`
or all interfaces, non-blocking, backlog 5. `get_max_players()` raises the
open-file limit and caps the player count at `CONFIG_MAX_PLAYING`, keeping
`NUM_RESERVED_DESCS` (8) descriptors spare.

`comm.c:new_descriptor` accepts at most one connection per loop pass:

1. `accept()`.
2. `tb_admit_connection()`: a fixed table of 256 IPv4 slots allows 12
   connections per address per 60-second window. Refused connections are
   closed before any allocation and are not logged, to avoid log
   amplification.
3. Non-blocking mode and the send buffer size.
4. If `CONFIG_MAX_PLAYING` descriptors exist, send "the game is full" and
   close.
5. Allocate the descriptor and resolve the host name with
   `gethostbyaddr()`, unless `CONFIG_NS_IS_SLOW` is set. **This lookup
   blocks the whole server** while DNS answers; set `nameserver_is_slow` in
   the configuration on hosts with slow resolvers.
6. Close the connection if the host matches a `BAN_ALL` entry.
7. `init_descriptor()`: small output buffer, history array, a descriptor
   number (`desc_num`, cycles 1 to 999), `ProtocolCreate()`, an empty event
   list, and the first state: `CON_GET_PROTOCOL` if
   `CONFIG_PROTOCOL_NEGOTIATION` is on, else `CON_GET_NAME`.
8. Prepend to `descriptor_list`.
9. With negotiation on, schedule `ePROTOCOLS` 1.5 seconds later, print
   "Attempting to Detect Client", and send `IAC DO TTYPE`. When the event
   fires, `interpreter.c:get_protocols` prints a one-line summary of what
   the client supports, then the greeting, and moves to `CON_GET_NAME`.
   Without negotiation the greeting is sent at once.

## Descriptor states and lifecycle

`STATE(d)` (the `connected` field) is one of the `CON_*` values in
`structs.h`. `CON_PLAYING` is 0. The OLC states run from `CON_OEDIT`
(`FIRST_OLC_STATE`) to `CON_IEDIT` (`LAST_OLC_STATE`).

- `IS_IN_OLC(d)` is true for the OLC range.
- `IS_PLAYING(d)` is true for `CON_PLAYING` and the OLC range: the
  character is in the world.
- `CON_CLOSE` means "close this connection; its character is not in the
  world". `close_socket()` frees the character.
- `CON_DISCONNECT` means "close this connection but leave its character in
  the world link-dead".

Code that wants to drop a connection should set one of the two states and
let the game loop call `close_socket()` at the end of the pass, rather than
closing it while something may still be iterating `descriptor_list`. Choose
by where the character is: a character in the world needs
`CON_DISCONNECT` (or must first be detached from the descriptor, as
`do_purge` does), otherwise it is freed while still linked into the world.

```
            accept
              |
   CON_GET_PROTOCOL --(1.5 s event)--+
              |                      |
         CON_GET_NAME <--------------+
          |        \
   known name     new name
          |          \
   CON_PASSWORD    CON_NAME_CNFRM -> CON_NEWPASSWD -> CON_CNFPASSWD
          |                                              |
          |                                  CON_QSEX -> CON_QCLASS
          |                                              |
          +---------------> CON_RMOTD <------------------+
                               |
                            CON_MENU --1--> CON_PLAYING <--> OLC states
                               |  \--2--> CON_PLR_DESC (editor) --> CON_MENU
                               |  \--4--> CON_CHPWD_GETOLD -> GETNEW -> VRFY
                               |  \--5--> CON_DELCNF1 -> CON_DELCNF2
                               '--0--> CON_CLOSE
```

`comm.c:check_idle_passwords` runs every 15 seconds and closes descriptors
that sit in `CON_GET_NAME` or `CON_PASSWORD` for a second check, so an idle
login lasts 15 to 30 seconds. `d->idle_tics` is reset only on entering
`CON_PASSWORD`.

## Input path

```
socket --read()--> read_buf --ProtocolInput()--> d->inbuf (in-band bytes)
   --process_input() line framing--> write_to_q() --> d->input queue
   --game_loop: get_from_q()--> pager | string_add | nanny | alias + command_interpreter
```

`comm.c:process_input` is called when `select()` reports the socket
readable:

1. It reads into a static buffer, at most the space left in `d->inbuf`
   (`MAX_RAW_INPUT_LENGTH`, 12 KB). If `inbuf` is full and holds no newline,
   the connection is closed ("input overflow").
2. `ProtocolInput()` removes telnet commands and subnegotiations and appends
   the remaining in-band bytes to `inbuf`.
3. It keeps reading until a newline arrives or the read would block.
4. For each complete line it builds a command of at most
   `MAX_INPUT_LENGTH - 1` (511) bytes:
   - backspace and DEL erase the previous character;
   - only printable ASCII is kept, so tabs, control bytes and 8-bit bytes
     are dropped (players therefore cannot type the `\t` colour escape;
     channels convert `@` instead, see `modify.c:parse_at`);
   - every `$` is doubled to `$$`, so player text that reaches an `act()`
     format prints a dollar sign instead of a substitution;
   - an over-long line is cut and the player is told what was kept.
5. History and shortcuts: `!` repeats the last command, `!text` repeats the
   most recent history entry starting with `text`, `^old^new` substitutes
   in the last command, and `--` discards every queued command. Ordinary
   lines are stored in a 5-entry history ring (`d->history`) and as
   `d->last_input`.
6. `write_to_q()` appends the line to `d->input`. At most 64 commands can
   wait; further lines are dropped.
7. Any partial line is moved to the start of `inbuf` for the next read.

Snoopers see each processed line with a `% ` prefix.

The game loop takes at most one line per descriptor per pulse; see
[overview.md](overview.md#the-game-loop) for the routing between the pager,
the editor, `nanny()` and the interpreter.

## Output path

All output for a descriptor goes through `comm.c:write_to_output` (or
`vwrite_to_output`, or `send_to_char`, which does nothing for a character
without a descriptor). Nothing is written to the socket until the loop's
output phase.

`vwrite_to_output` works like this:

1. If the descriptor is in the overflow state (`bufspace == 0`), the text is
   dropped.
2. The text is formatted into a static `MAX_STRING_LENGTH` (48 KB) buffer.
3. `ProtocolOutput()` expands colour, MXP and unicode codes into its own
   static buffer, which is copied back.
4. The result is appended to the output buffer:
   - Each descriptor starts with an inline `small_outbuf` of
     `SMALL_BUFSIZE` (1 KB).
   - When the text does not fit, the descriptor takes a large buffer of
     `LARGE_BUFSIZE` bytes (about 24 KB) from the shared pool `bufpool`
     (or allocates one), copies the small buffer into it and continues
     there.
   - When even the large buffer cannot hold the text, the text is cut,
     `bufspace` reaches 0, and the descriptor enters the overflow state.

`comm.c:process_output` runs for each descriptor with pending output whose
socket is writable. It assembles one block of at most `MAX_SOCK_BUF` bytes:
a leading `\r\n` (used only if the player was sitting at a prompt), the
buffered output, `**OVERFLOW**` if output was lost, an extra blank line for
players without `PRF_COMPACT`, and a new prompt. `write_to_descriptor()`
writes as much as the kernel accepts. If everything went out, the large
buffer returns to the pool and the descriptor switches back to its small
buffer; otherwise the unsent remainder is kept for the next pass. A
snooper receives a copy of the output (on the base branch, including bytes
not yet sent; see [open problems](#open-problems)).

`make_prompt()` builds the prompt: the pager prompt while paging, `] `
while in the string editor, and otherwise the game prompt (invisibility
level, hit/mana/move according to `PRF_DISPHP`/`DISPMANA`/`DISPMOVE` or
`PRF_DISPAUTO`, `BUILDWALKING`, `AFK`, `(news)`, `(motd)`, then `> `).
Descriptors that produced no output still get a prompt each pass if they do
not have one.

`protocol.c:Write` sends negotiation bytes through `write_to_output` and sets
`pProtocol->WriteOOB` when the buffer was empty, so `process_output` sends
that block without the CRLF, blank line and prompt decoration. The game loop
clears `WriteOOB` before reading input.

Invariants:

- `vwrite_to_output` and `ProtocolOutput` use static buffers. Copy the
  result of `ProtocolOutput()` before calling it again.
- `t->output` points either at `t->small_outbuf` or at
  `t->large_outbuf->text`. A large buffer belongs to exactly one descriptor
  while `large_outbuf` points at it; once it is back on `bufpool` the
  descriptor must not write to it. Only `close_socket()` may call
  `flush_queues()`, which returns the buffer.
- Pool buffers are never freed while the server runs; `free_bufpool()` frees
  them at shutdown.
- `write_to_descriptor()` writes directly to the socket, bypassing the
  buffer. It is used for prompts, emergency messages and copyover; the
  descriptor's buffered output can then arrive after it.

### Broadcast helpers and `act()`

`send_to_all`, `send_to_outdoor`, `send_to_room`, `send_to_group`,
`send_to_range` and `game_info` loop over descriptors or room occupants and
call `vwrite_to_output`. `send_to_room()` does not validate the rnum.

`act()` is a macro for `comm.c:act_at`, which records the caller's file,
line and function for diagnostics and calls `act_body()`. `act_body()`
picks recipients (`TO_CHAR`, `TO_VICT`, `TO_ROOM`, `TO_NOTVICT`,
`TO_GMOTE`, plus the `TO_SLEEP` and `DG_NO_TRIG` flags) and calls
`perform_act()` for each, which expands the `$` codes (`$n`, `$N`, `$e`,
`$m`, `$s`, `$o`, `$p`, `$a`, `$t`, `$T`, `$F`, `$u`, `$U`, `$$`) into a
`MAX_STRING_LENGTH` buffer, delivers it, and **runs the recipient's act
triggers synchronously** (`act_mtrigger`) when the recipient is an NPC. A
NULL or empty message is the documented "no message" case and returns NULL.
`act()` returns the last message it built (`last_act_message`), which the
channel history uses. Because act triggers can move or purge characters,
the room-recipient walk is unsafe on the base branch (see
[open problems](#open-problems)).

## Closing a connection

`comm.c:close_socket` tears a descriptor down in this order:

1. Unlink it from `descriptor_list`, close the socket, return the large
   output buffer to the pool and free the input queue (`flush_queues`).
2. Break snoop links in both directions.
3. If there is a character: clear `ch->desc`; free a mail message being
   written, or the description backup; record a `LAST_DISCONNECT` entry. If
   the descriptor was playing (or in OLC, or `CON_DISCONNECT`), announce the
   lost link, `save_char()` the real body and leave it in the world.
   Otherwise `free_char()` it.
4. If an immortal was switched, clear the original body's `desc`.
5. Free the command history and any pager buffers.
6. `ProtocolDestroy()`.
7. Cancel the descriptor's mud events.
8. `cleanup_olc()` for an editor state (see [olc.md](olc.md)).
9. `free(d)`.

Callers: the game loop (exception set, input or output error, the
`CON_CLOSE`/`CON_DISCONNECT` sweep), the shutdown in `init_game()`, and
copyover. After `close_socket()` returns, `d` is freed; a loop over
`descriptor_list` must have saved `d->next` first.

## Telnet protocols, MSDP and colour

`protocol.c` is KaVir's public-domain protocol snippet (snippet version 6).
Each descriptor owns a `protocol_t` (`d->pProtocol`) created by
`ProtocolCreate()` and freed by `ProtocolDestroy()`; it records what the
client supports, the screen size, the client name and the MSDP variables.

### Negotiation

`ProtocolNegotiate()` sends only `IAC DO TTYPE`, because clients that do not
understand telnet print anything printable. When the client answers
`WILL TTYPE` or `WONT TTYPE`, `PerformHandshake()` marks it negotiated and
`Negotiate()` offers the rest:

| Option | Server sends | Meaning |
| --- | --- | --- |
| TTYPE (24) | `DO`, then `SB TTYPE SEND` | client name; cycled to detect 256 colours and MTTS flags |
| NAWS (31) | `DO` | window width and height |
| CHARSET (42) | `DO`, then a UTF-8 request | UTF-8 support |
| MSDP (69) | `WILL` | out-of-band variables (see below) |
| MSSP (70) | `WILL` | server status for MUD crawlers (`SendMSSP()`) |
| ATCP (200) | `DO` | MSDP fallback for older clients |
| MSP (90) | `WILL` | sound triggers |
| MXP (91) | `DO`, then `WILL` if refused | MUD eXtension Protocol links and tags |
| MCCP (86) | only with `USING_MCCP` | compression; not implemented (`CompressStart()` only logs) |

### Input parsing

`ProtocolInput()` walks the bytes just read. Outside a subnegotiation it
copies in-band bytes to the output, turns `IAC IAC` into a single 255 byte,
answers `DO`/`DONT`/`WILL`/`WONT` through `PerformHandshake()`, and
recognises an MXP `ESC [ n z` version reply. Inside `IAC SB ... IAC SE` it
collects the data and then calls `PerformSubnegotiation()` (TTYPE, NAWS,
CHARSET, MSDP, ATCP). The per-descriptor `bIACMode` flag remembers an open
subnegotiation between reads, but the collected bytes live in a static
buffer that is reset on every call, so a subnegotiation split across two
reads loses its first part. The function returns the number of in-band
bytes it copied.

### MSDP

`VariableNameTable[]` in `protocol.c` lists the variables (character,
combat, world, configuration and GUI). The client asks with `REPORT`,
`SEND`, `LIST` and friends; `comm.c:msdp_update` sets the character values
once per second for every playing descriptor and calls `MSDPUpdate()`,
which sends the dirty variables that the client asked to be reported. ATCP
is used instead when the client does not speak MSDP.

### Colour and MXP codes

The engine writes colour with a tab escape: `\t` followed by a code.
`ProtocolOutput()` translates it according to what the client supports
(ANSI, xterm 256 colours, or nothing):

| Code | Meaning |
| --- | --- |
| `\tn` | reset |
| `\tr` `\tg` `\ty` `\tb` `\tm` `\tc` `\tw` `\to` `\tp` `\td` `\ta` | dark red, green, yellow, blue, magenta, cyan, white, orange, pink, grey, azure |
| upper case of the above | bright variants |
| `\t1` `\t2` `\t3` | the MUD's palette colours (`RGBone`..`RGBthree`) |
| `\t[F123]` / `\t[B123]` | RGB foreground/background, digits 0 to 5 |
| `\t[U9814/C]` | unicode character, with ASCII fallback text |
| `\t(` ... `\t)` | MXP `<send>` link |
| `\t<tag>` | raw MXP tag (stripped when MXP is off) |
| `\t[X1.0]` | require an MXP version for the next tag |
| `\t_` `\t+` `\t-` `\t=` | underline, bold, blink, reverse |
| `\t*` | a literal `@` |
| `\t!` | MSP sound trigger |
| `\t\t` | a literal tab |

World files, help and text files store colour with `@` (for example
`@rred@n`); `modify.c:parse_at` converts `@` to `\t` when they are loaded,
and `parse_tab` converts back for saving and editing. Players can use `@`
codes on channels when `CONFIG_SPECIAL_IN_COMM` is on; the channel code in
`act.comm.c` refuses the MXP brackets.

`ProtocolOutput()` writes into a static buffer of `MAX_OUTPUT_BUFFER`
(`LARGE_BUFSIZE`) bytes. If the expanded text would not fit, **it returns an
empty string** and logs "Too much outgoing data"; the text is lost rather
than truncated.

### Copyover state

`CopyoverGet()` encodes the screen size and the negotiated options as a
short string for `copyover.dat`; `CopyoverSet()` restores them and
renegotiates MSDP/ATCP and MXP, because a client does not know a copyover
happened.

## The login state machine

`interpreter.c:nanny` handles every state except `CON_PLAYING`. OLC states
are dispatched first, through a table of `*_parse` functions. The login
states:

| State | Input | Action |
| --- | --- | --- |
| `CON_GET_PROTOCOL` | anything | "Collecting Protocol Information"; the `ePROTOCOLS` event moves on |
| `CON_GET_NAME` | name | empty input closes. The name must be letters only, 2 to 20 characters, pass `valid_name()` and not be a fill or reserved word. A known name goes to `CON_PASSWORD` (echo off); a deleted one is removed and treated as new; an unknown one goes to `CON_NAME_CNFRM`. |
| `CON_NAME_CNFRM` | Y/N | `BAN_NEW` sites and `-r` (wizlock) refuse new characters; `perform_new_char_dupe_check()` boots another connection creating the same name |
| `CON_PASSWORD` | password | rate limit, verify, upgrade legacy hashes, site and wizlock checks, `perform_dupe_check()`, motd, `CON_RMOTD` |
| `CON_NEWPASSWD`, `CON_CHPWD_GETNEW` | password | 3 to `MAX_PWD_LENGTH` (30) characters, not the name; hashed at once |
| `CON_CNFPASSWD`, `CON_CHPWD_VRFY` | password again | must verify against the new hash |
| `CON_QSEX`, `CON_QCLASS` | choice | new characters: sex, class, then `create_entry()`, `init_char()`, `save_char()`, `save_player_index()` |
| `CON_RMOTD` | return | main menu |
| `CON_MENU` | 0 to 5 | 0 quit, 1 enter the game, 2 edit description, 3 read the background, 4 change password, 5 delete character |
| `CON_DELCNF1`, `CON_DELCNF2` | password, then `yes` | sets `PLR_DELETED` (below `LVL_GRGOD`), deletes rent and script variables, and with `selfdelete_fastwipe` removes the files at once |

### Passwords

New and changed passwords are hashed with Argon2id from libsodium
(`crypto_pwhash_str_alg()` with the `INTERACTIVE` limits) into the
128-byte `passwd` field. Verification (`tb_password_verify()`) accepts an
Argon2id string or, when the build has `crypt()`, a legacy crypt hash; there
is no plaintext fallback. A successful login with a legacy hash re-hashes
the password with Argon2id and saves the player.

Failed logins are limited in three ways: a shared 512-slot table allows 8
failures per character name per 60 seconds no matter which address they
come from; each connection is closed after `CONFIG_MAX_BAD_PWS` failures;
and the character's persistent `Badp` count is shown at the next successful
login. Hashing and verification run synchronously in the game loop.

### Duplicate logins

`perform_dupe_check()` runs after a correct password. It closes every other
descriptor controlling the same idnum and picks a body to reconnect to:

- `UNSWITCH`: an immortal was switched out of this body;
- `USURP`: another connection was playing this character and is closed;
- `RECON`: the character is link-dead in the world.

Any further copies in `character_list` are moved to room rnum 1 and
extracted. If a body is found, the freshly loaded character is freed and
the descriptor takes over the body in `CON_PLAYING`.

### Entering the game

`interpreter.c:enter_player_game` (also used by copyover) resets the
character, picks the load room (the saved one if it still exists, else the
immortal or mortal start room; frozen players go to the frozen room), sets
`script_id` to the idnum and registers it in the UID table, loads saved
script variables, links the character into `character_list` and the room,
loads rented objects (`Crash_load()`), saves, and fires the room's login
trigger. `nanny()` then fires greet triggers, announces the arrival, runs
`do_start()` for a level-0 character, and shows the room.

## The command interpreter

### The command table

`interpreter.c:cmd_info[]` lists every command:

```c
struct command_info {
   const char *command;      /* full name */
   const char *sort_as;      /* key for the sorted 'commands' listing */
   byte minimum_position;    /* POS_* needed */
   void (*command_pointer)(struct char_data *ch, char *argument, int cmd, int subcmd);
   sh_int minimum_level;     /* level needed */
   int subcmd;               /* passed through to the ACMD */
};
```

Entry 0 is `RESERVED` (special procedures use command 0) and the list ends
with `"\n"`. The direction commands come first. **Order is priority**: a
typed word matches the first command it is a prefix of (for which the
player has the level), so `n` means `north`. `act.social.c:create_command_list`
builds `complete_cmd_info[]`, the merged table of commands and socials that
the interpreter actually searches; `sort_commands()` fills `cmd_sort_info`
for listings.

`ACMD(name)` declares a command handler:
`void name(struct char_data *ch, char *argument, int cmd, int subcmd)`.
`argument` points into the caller's line buffer and may be modified in place
by the handler. `cmd` indexes `complete_cmd_info`; `CMD_IS("name")` compares
it with a command name.

### `command_interpreter()`

1. Clear `AFF_HIDE` and skip leading spaces; an empty line does nothing.
2. A line starting with a non-letter is a one-character command (`'hello`,
   `;wiznet`); otherwise the first word is the command.
3. Command triggers: room (`command_wtrigger`), then mobiles, then objects.
   If one handles the command, stop. Command triggers do their own target
   checks (`valid_dg_target`).
4. An implementor switched into a mobile can use the mobile script commands
   (`mecho`, `mload`, ...) through `script_command_interpreter()`.
5. Search `complete_cmd_info` for the first non-social command matching the
   prefix at the player's level, then for a social.
6. Refuse: unknown command (prints `CONFIG_HUH` and suggests commands within
   Levenshtein distance 2), frozen player, unimplemented command, immortal
   command used by a switched mobile, position too low.
7. `special()`: give special procedures a chance, in this order: the room,
   worn equipment, carried objects, mobiles in the room (skipping those
   pending extraction), objects in the room. The first one that returns
   non-zero consumes the command. `-s` disables this step.
8. Call the handler.

The interpreter is also called for forced commands (`force`, DG `%force%`
and `mforce`). Those lines do not pass through `process_input()`, so they
can be longer than a typed line; the argument helpers below bound their
output.

## Argument parsing helpers

All output buffers must hold `MAX_INPUT_LENGTH` (512) bytes; longer words
are cut and reported as an `interpreter`/`parse-argument` diagnostic.

| Function | Behaviour |
| --- | --- |
| `one_argument(arg, first)` | skip spaces, copy one word **in lower case**, skip fill words (`in from with the on at to`); returns the rest |
| `any_one_arg(arg, first)` | same, without skipping fill words |
| `one_word(arg, first)` | like `any_one_arg`, but a `"quoted phrase"` is one word |
| `two_arguments(arg, a, b)` | two `one_argument` calls |
| `half_chop(str, a, b)` | first word to `a`, the rest (spaces skipped) to `b`; `b` may be the same buffer as `str`, `a` may not |
| `skip_spaces(&p)` | skips white space but stops at a tab, which starts a colour code |
| `search_block(arg, list, exact)` | index of `arg` in a `"\n"`-terminated list, or -1; **lower-cases `arg` in place**; an argument starting with `!` never matches |
| `is_abbrev(a, b)` | `a` is a non-empty case-insensitive prefix of `b` |
| `is_number(s)` | optional `-` then digits only |
| `find_command(name)` | exact match in `complete_cmd_info` |
| `delete_doubledollar(s)` | undo the input `$$` doubling, in place, for text that will not go through `act()` |

## Aliases

Each player has a list of `struct alias_data` (`GET_ALIASES(ch)`), saved in
the player file. `do_alias` adds, replaces or removes one; you cannot alias
`alias`. A replacement containing `;` or `$` is *complex*.

`perform_alias()` runs before `command_interpreter()` for lines typed by the
player. A simple alias replaces the line in place. A complex alias is
expanded by `perform_complex_alias()`: `;` separates commands, `$1` to `$9`
insert words of the original line, `$*` inserts all of it, and other `$`
characters are doubled. The resulting commands are pushed onto the front of
the input queue marked as aliased, so they are not expanded again. An
expansion longer than `MAX_RAW_INPUT_LENGTH` is discarded with "Alias
expansion too long".

## Site bans and name rules

`ban.c` keeps `ban_list`, loaded from `lib/etc/badsites`. Each line is
`type site date name`, where type is `new`, `select` or `all`. A host matches
an entry when the entry's site is a substring of the lower-cased host name.

| Type | Effect |
| --- | --- |
| `BAN_NEW` | no new characters from the site |
| `BAN_SELECT` | only characters with `PLR_SITEOK` may log in |
| `BAN_ALL` | connections are refused in `new_descriptor()` |

`isbanned()` lower-cases the host string it is given, in place, and matches
it against sites that are stored lower-case: `do_ban` lower-cases the site it
is given, and `load_banned()` lower-cases a site from the file and reports
one that had uppercase letters (`ban/load-site-case`). `do_ban` and
`do_unban` (level `LVL_GRGOD`) edit the list and rewrite the file. `SIGUSR2`
clears the bans, the invalid-name list and the wizlock in an emergency.

`valid_name()` rejects names without a vowel, names with spaces, and names
containing any substring listed in `lib/misc/xnames` (at most 200 entries;
a file with 200 or more entries stops the boot).

## The string editor

The same editor serves mail, boards, ideas/bugs/typos, the character
description, OLC text fields and text files.

### Starting

`modify.c:string_write(d, &target, max, mailto, backup)` starts an editing
session:

- `d->str` is set to `&target`, the `char *` the text will be written to
  (for example `&ch->player.description` or an OLC scratch field);
- `d->max_str` limits the length;
- `d->mail_to` is the mail recipient idnum, or `BOARD_MAGIC + board` for a
  board post;
- `backup` becomes `d->backstr`, the text restored on abort (the improved
  editor is always compiled in);
- `PLR_WRITING` is set, which keeps channel messages and most `act()` text
  away from the writer.

While `d->str` is set, every input line goes to `string_add()` instead of
the interpreter, and the prompt is `] `.

### Each line

`modify.c:string_add`:

1. `delete_doubledollar()` and `smash_tilde()`: a `~` at the end of a line
   becomes a space, because `~` ends strings in the world and player files.
2. A line starting with `/` is an editor command
   (`improved-edit.c:improved_editor_execute`):

   | Command | Action |
   | --- | --- |
   | `/s` | save and finish |
   | `/a` | abort and restore the old text |
   | `/c` | clear the buffer |
   | `/l`, `/n` | list the buffer, plain or with line numbers |
   | `/d#` | delete line(s) |
   | `/i# text` | insert a line |
   | `/e# text` | replace a line |
   | `/r 'a' 'b'`, `/ra 'a' 'b'` | replace the first or every occurrence |
   | `/f`, `/fi` | reformat the text (with indent); trigger scripts use `format_script()` |
   | `/t` | toggle colour codes between `@` and tab form |
   | `/h` | help |

3. Otherwise the line is appended with `\r\n`. A line that would exceed
   `d->max_str` is skipped with a message.

### Finishing

On save or abort, `string_add()`:

- on abort, for the description, OLC and IBT states, frees the new text and
  puts `d->backstr` back into `*d->str`;
- on save, replaces an empty result with `"Nothing.\r\n"` and frees
  `d->backstr`;
- calls the state's cleanup function (`cedit_string_cleanup`,
  `medit_string_cleanup`, `oedit_string_cleanup`, `redit_string_cleanup`,
  `tedit_string_cleanup`, `trigedit_string_cleanup`,
  `hedit_string_cleanup`, `qedit_string_cleanup`, `ibtedit_string_cleanup`,
  `exdesc_string_cleanup` for `CON_PLR_DESC`, `playing_string_cleanup` for
  `CON_PLAYING`);
- clears `d->str`, `d->mail_to`, `d->max_str` and the `PLR_WRITING`,
  `PLR_MAILING`, `PLR_BUG`, `PLR_IDEA` and `PLR_TYPO` flags.

`playing_string_cleanup()` stores mail (`store_mail()`; for mail `d->str`
itself was allocated and is freed here), saves the board for a board post
(an aborted post stays posted), and saves or discards an idea, bug or typo.
`close_socket()` frees an unfinished mail message and a description backup.

## The pager

`modify.c:page_string(d, str, keep_internal)` splits long text into pages
of the player's page length and screen width (colour codes do not count)
and shows the first page. `d->showstr_vector` holds a pointer to the start
of each page and `d->showstr_count` the number of pages. While paging,
input goes to `show_string()`: Return for the next page, `q` to quit, `r`
to redisplay, `b` to go back, or a page number.

Ownership: with `keep_internal` set, the text is copied to
`d->showstr_head`, which the descriptor owns. Without it, the page pointers
point into the caller's string, **which must stay allocated and unchanged
until the player finishes paging**. Static and global text is safe only as
long as nothing frees it; `db.c:file_to_string_alloc` checks for an active
pager before replacing a text file. `close_socket()` and `show_string()`
free the vector and the internal copy.

## Open problems

Problems found while writing this document, on the base branch
(`audit/macro-inventory-and-dg-followup-20261027`). Fixed elsewhere means a
commit on the named branch that has not been merged here.

- `act.wizard.c:do_dc` sets `CON_CLOSE` for any descriptor not in
  `CON_PLAYING`, including the OLC states, whose character is in the world.
  `close_socket()` then frees that character while it is still linked into
  its room and `character_list`, and skips `cleanup_olc()`. Fixed on the
  integrated tree (commit `1caeebc`): every in-game state (`IS_PLAYING`)
  takes the link-loss path, `CON_DISCONNECT`, after the editor is left as
  its own exit would leave it (a text being written is abandoned as `/a`
  does, then `cleanup_olc()` releases the workspace); only out-of-game
  sockets get `CON_CLOSE`, and a refused target is reported as `dc-denied`.
- `objsave.c:Crash_listrent` and `house.c:House_listrent` call
  `page_string(ch->desc, buf, 0)` on a local array. A listing longer than
  one page leaves `showstr_vector` pointing into a dead stack frame. The
  pager also keeps pointers into help entries (`do_help`) and room extra
  descriptions (`look`) that `hedit`, `reload xhelp` and `redit` can free.
  Fixed on the integrated tree: the rent and house listings (`dbc30ab`)
  and the help entry and extra description (`34f87c2`) are paged from a
  copy the pager owns.
- `comm.c:close_socket` runs `cleanup_olc()` only for some editor states;
  `CON_CEDIT`, `CON_PREFEDIT`, `CON_IBTEDIT` and `CON_IEDIT` leak their
  workspace. Fixed on the integrated tree (F3-11): any attached workspace
  is released whatever the state; `tests/test_close_socket.c` pins all four.
- `ProtocolInput()` counts NUL bytes in its return value, but `strlcat()`
  stops at the first NUL, so `process_input()` advances past bytes it never
  received. Input after a NUL byte in the same read is lost or framed
  against stale buffer contents (memory-safe, but wrong).
- `vwrite_to_output()` passes `(int *)&wantsize`, a `size_t`, to
  `ProtocolOutput()`. Fixed on `audit-deferred-errloc` /
  `worktree-wf_c613c2c5-f9f-28` (commit `ca880b2`) and on
  `audit-fix-infra-findings` (commit `c850b4b`).
- `ProtocolOutput()` reads one byte past the string when the text ends in
  a tab. Found by the protocol fuzzer; fixed on `audit-fix-infra-findings`
  (commit `91d3d54`).
- `msdp_update()` divides by the opponent's `max_hit`, which is 0 for a
  mobile with `0d0+0` hit dice: `SIGFPE` kills the server. Fixed on
  `audit-deferred-errloc` / `worktree-wf_c613c2c5-f9f-28` (commit
  `10f5434`).
- The `TO_ROOM`/`TO_NOTVICT` loop in `act_body()` follows
  `to->next_in_room` after act triggers ran, and a nested
  `act(..., DG_NO_TRIG)` resets the outer call's trigger flag. Fixed on
  `audit-deferred-errloc` (commit `8773ab9`) and
  `worktree-wf_c613c2c5-f9f-25` (commit `674f22f`).
- `send_to_range()` stops before `top_of_world`, so the highest room never
  receives range echoes. Fixed on `audit-deferred-errloc` (commit
  `157d7d6`).
- `process_output()` can leave `bufptr` negative after a partial prompt
  write, and an oversized write with a large buffer already active leaks
  that buffer. Fixed on `worktree-wf_c613c2c5-f9f-25` (commits `292632a`,
  `be0c08f`).
- `new_descriptor()` never compares the new file descriptor with
  `FD_SETSIZE`, and `copyover_recover()` trusts every descriptor number in
  `copyover.dat`. Fixed on `worktree-wf_c613c2c5-f9f-25` (commits
  `2c93a8b`, `d014e9e`).
- The snoop copy in `process_output()` uses `%*s` (a field width, not a
  precision), so the snooper sees unsent bytes. Fixed on
  `worktree-wf_c613c2c5-f9f-25` (commit `0084b5f`).
- Drops are silent: commands beyond the 64-line queue, refused
  connections, and (until fixed) the "line too long" notice, which only
  fires when the last copied character is a doubled `$`. Reporting is added
  on `worktree-wf_c613c2c5-f9f-25` (commits `fa1ff4b`, `9204483`,
  `6cc3ba6`).
- The `SIGUSR2` emergency unban leaks the ban nodes and invalid names it
  clears. Fixed on `audit-deferred-errloc` (commit `0f4c118`).
- `ban.c:valid_name` tries to refuse a name another connection is creating
  by looking for an idnum of -1, but `db.c:clear_char` zeroes the idnum, so
  that check never matches. `perform_new_char_dupe_check()` covers the case
  later in the login.
