/* Shared between fuzz_player.c (the harness) and fuzz_player_stubs.c (the
 * output sinks and game-wide stand-ins it links with). */
#ifndef FUZZ_PLAYER_H
#define FUZZ_PLAYER_H

/* Read a whole string, so ASan sees an unterminated buffer. */
void fuzz_sink_string(const char *str);

/* The server log (utils.c logfile) is an in-memory stream: the real logger
 * formats every line, nothing reaches the disk, and it is rewound before
 * each input so it stays bounded. */
void fuzz_log_open(void);
void fuzz_log_rewind(void);

#endif
