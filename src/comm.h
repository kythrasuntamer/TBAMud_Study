/**
* @file comm.h
* Header file, prototypes of public communication functions.
*
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*
* All rights reserved.  See license for complete information.
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.
*
*/
#ifndef TBAMUD_COMM_H_INCLUDED
#define TBAMUD_COMM_H_INCLUDED

#define NUM_RESERVED_DESCS	8
#define COPYOVER_FILE "copyover.dat"

/* Stable identity of a connection in diagnostics: descriptor number, socket
 * and the attached character (mob vnum or player idnum, "no-char -1" before
 * login). The host name is deliberately absent: it comes from reverse DNS,
 * which the remote side controls. Use as a pair, e.g.
 *   TB_WARN_MSG("network", "input-flood", "write_to_q",
 *               DIAG_DESC_FMT " dropped", DIAG_DESC(d));
 * DIAG_DESC evaluates d several times: pass a plain, non-NULL variable. */
#define DIAG_DESC_FMT "desc #%d fd %ld %s %ld"
#define DIAG_DESC(d) (d)->desc_num, (long) (d)->descriptor, \
  ((d)->character ? (IS_NPC((d)->character) ? "mob vnum" : "player idnum") \
                  : "no-char"), \
  ((d)->character ? (IS_NPC((d)->character) \
                     ? (long) GET_MOB_VNUM((d)->character) \
                     : (long) GET_IDNUM((d)->character)) : -1L)

/* Rate-limited TB_WARN_MSG / TB_ERROR_MSG for conditions that recur on
 * every pulse or that a remote peer can trigger at will (input floods,
 * connection storms, bad data read once per second). The first report from
 * a call site is logged at once; later ones within 'seconds' are counted,
 * and the count is logged with the next report that gets through, so
 * nothing is dropped silently but the log cannot be flooded. State is per
 * call site (a static pair inside the expansion). The variadic arguments
 * are evaluated only when a record is written. */
#define TB_LOG_EVERY_(severity, seconds, subsystem, operation, resource, ...) do { \
  static time_t tb_we_last_; \
  static unsigned long tb_we_suppressed_; \
  time_t tb_we_now_ = time(NULL); \
  if (tb_we_last_ == 0 || tb_we_now_ - tb_we_last_ >= (seconds)) { \
    TB_ERROR_LOG((severity), (subsystem), (operation), (resource), 0, \
                 __VA_ARGS__); \
    if (tb_we_suppressed_) \
      TB_ERROR_LOG((severity), (subsystem), (operation), (resource), 0, \
                   "%lu similar reports suppressed in the last %ld s", \
                   tb_we_suppressed_, (long) (tb_we_now_ - tb_we_last_)); \
    tb_we_last_ = tb_we_now_; \
    tb_we_suppressed_ = 0; \
  } else \
    ++tb_we_suppressed_; \
} while (0)
#define TB_WARN_EVERY(seconds, subsystem, operation, resource, ...) \
  TB_LOG_EVERY_("WARNING", (seconds), (subsystem), (operation), (resource), \
                __VA_ARGS__)
#define TB_ERROR_EVERY(seconds, subsystem, operation, resource, ...) \
  TB_LOG_EVERY_("ERROR", (seconds), (subsystem), (operation), (resource), \
                __VA_ARGS__)

/* comm.c */
void close_socket(struct descriptor_data *d);
void game_info(const char *messg, ...) __attribute__ ((format (printf, 1, 2)));
size_t send_to_char(struct char_data *ch, const char *messg, ...) __attribute__
    ((format (printf, 2, 3)));
void send_to_all(const char *messg, ...) __attribute__ ((format (printf, 1,
    2)));
void send_to_room(room_rnum room, const char *messg, ...) __attribute__ ((format
    (printf, 2, 3)));
void send_to_outdoor(const char *messg, ...) __attribute__ ((format (printf, 1,
    2)));
void send_to_group(struct char_data *ch, struct group_data *group, const char * msg, ...) __attribute__ ((format
    (printf, 3, 4)));
void send_to_range(room_vnum start, room_vnum finish, const char *messg, ...)
    __attribute__ ((format (printf, 3, 4)));

/* Act type settings and flags */
#define TO_ROOM     1   /**< act() type: to everyone in room, except ch. */
#define TO_VICT     2   /**< act() type: to vict_obj. */
#define TO_NOTVICT  3   /**< act() type: to everyone in room, not ch or vict_obj. */
#define TO_CHAR     4   /**< act() type: to ch. */
#define TO_GMOTE    5   /**< act() type: to gemote channel (global emote) */
#define TO_SLEEP    128	/**< act() flag: to char, even if sleeping */
#define DG_NO_TRIG  256 /**< act() flag: don't check act trigger   */


/* act functions */
void perform_act(const char *orig, struct char_data *ch, struct obj_data *obj, void *vict_obj, struct char_data *to);
/* act() is a macro so every message records the file, line and function of
 * the code that sent it; formatting diagnostics in perform_act() report that
 * call site. Call act() exactly as before. A NULL or empty str means "no
 * message" (socials use it for absent entries) and returns NULL silently. */
char *act_at(const char *src_file, int src_line, const char *src_func,
             const char *str, int hide_invisible, struct char_data *ch,
             struct obj_data *obj, void *vict_obj, int type);
#define act(str, hide_invisible, ch, obj, vict_obj, type) \
  act_at(__FILE__, __LINE__, __func__, (str), (hide_invisible), (ch), (obj), \
         (vict_obj), (type))

/* I/O functions */
int	write_to_q(const char *txt, struct txt_q *queue, int aliased);
int	write_to_descriptor(socket_t desc, const char *txt);
size_t	write_to_output(struct descriptor_data *d, const char *txt, ...) __attribute__ ((format (printf, 2, 3)));
/* va_list form of write_to_output(); format(printf, 2, 0) marks it as
 * forwarding a caller's checked format. */
size_t	vwrite_to_output(struct descriptor_data *d, const char *format, va_list args)
  __attribute__ ((format (printf, 2, 0)));

typedef RETSIGTYPE sigfunc(int);

void echo_off(struct descriptor_data *d);
void echo_on(struct descriptor_data *d);
void game_loop(socket_t mother_desc);
void heartbeat(int heart_pulse);
void copyover_recover(void);

extern struct descriptor_data *descriptor_list;
extern int buf_largecount;
extern int buf_overflows;
extern int buf_switches;
extern int circle_shutdown;
extern int circle_reboot;
extern int no_specials;
extern int scheck;
extern FILE *logfile;
extern unsigned long pulse;
extern ush_int port;
extern socket_t mother_desc;
extern int next_tick;

#endif /* TBAMUD_COMM_H_INCLUDED */
