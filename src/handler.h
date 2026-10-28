/**
* @file handler.h
* Prototypes of handling and utility functions.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               
*/
#ifndef TBAMUD_HANDLER_H_INCLUDED
#define TBAMUD_HANDLER_H_INCLUDED

/* handling the affected-structures */
void	affect_total(struct char_data *ch);
void	affect_to_char(struct char_data *ch, struct affected_type *af);
void	affect_remove(struct char_data *ch, struct affected_type *af);
void	affect_from_char(struct char_data *ch, int type);
bool	affected_by_spell(struct char_data *ch, int type);
void	affect_join(struct char_data *ch, struct affected_type *af,
bool add_dur, bool avg_dur, bool add_mod, bool avg_mod);

/* utility */
const char *money_desc(int amount);
struct obj_data *create_money(int amount);
int	isname(const char *str, const char *namelist);
int	is_name(const char *str, const char *namelist);
char	*fname(const char *namelist);
int	get_number(char **name);

/* objects */
void	obj_to_char(struct obj_data *object, struct char_data *ch);
void	obj_from_char(struct obj_data *object);

/** Wear obj on ch at pos. A call-site-recording macro (like act()), so a
 * rejection names the caller (zone reset, script, objsave). Preconditions:
 * obj is in no location (not carried, in a room, in a container or worn)
 * and pos is in [0, NUM_WEARS). On an alignment/class zap the object goes
 * to ch's inventory. On any other rejection (slot taken, bad pos, obj still
 * placed) an equip-reject error is reported, and a free object is put in
 * ch's inventory rather than orphaned; a placed object stays where it is.
 * The caller is never told which happened. */
void	equip_char_at(const char *src_file, int src_line, const char *src_func,
                      struct char_data *ch, struct obj_data *obj, int pos);
#define equip_char(ch, obj, pos) \
  equip_char_at(__FILE__, __LINE__, __func__, (ch), (obj), (pos))
struct obj_data *unequip_char(struct char_data *ch, int pos);
int	invalid_align(struct char_data *ch, struct obj_data *obj);

void	obj_to_room(struct obj_data *object, room_rnum room);
void	obj_from_room(struct obj_data *object);
void	obj_to_obj(struct obj_data *obj, struct obj_data *obj_to);
void	obj_from_obj(struct obj_data *obj);
void	object_list_new_owner(struct obj_data *list, struct char_data *ch);

void	extract_obj(struct obj_data *obj);

void update_char_objects(struct char_data *ch);

/* characters*/
struct char_data *get_char_room(char *name, int *num, room_rnum room);
struct char_data *get_char_num(mob_rnum nr);

/* Room movement primitives. Call-site-recording macros (like act()), so an
 * invariant failure names the originating caller. char_from_room() aborts
 * on a NULL or roomless character; char_to_room() rejects and reports an
 * invalid room, leaving the character in NOWHERE. */
void	char_from_room_at(const char *src_file, int src_line,
                          const char *src_func, struct char_data *ch);
void	char_to_room_at(const char *src_file, int src_line,
                        const char *src_func, struct char_data *ch,
                        room_rnum room);
#define char_from_room(ch) \
  char_from_room_at(__FILE__, __LINE__, __func__, (ch))
#define char_to_room(ch, room) \
  char_to_room_at(__FILE__, __LINE__, __func__, (ch), (room))
void	extract_char(struct char_data *ch);
void	extract_char_final(struct char_data *ch);
void	extract_pending_chars(void);

/* Room occupant snapshots, for loops whose body runs callbacks (cast,
 * damage and death triggers) that can move, purge or extract any occupant,
 * so next_in_room must not be kept across the body. room_people_uids()
 * returns the number of occupants and a CREATE'd array of their UIDs in
 * *uids (NULL when empty; the caller frees it); an invalid room is reported
 * at the caller and yields 0. room_person_live() re-resolves one UID: the
 * character if it is still in room and not pending extraction, else NULL. */
int	room_people_uids_at(const char *src_file, int src_line,
                            const char *src_func, room_rnum room, long **uids);
#define room_people_uids(room, uids) \
  room_people_uids_at(__FILE__, __LINE__, __func__, (room), (uids))
struct char_data *room_person_live(long uid, room_rnum room);

/* find if character can see */
struct char_data *get_player_vis(struct char_data *ch, char *name, int *number, int inroom);
struct char_data *get_char_vis(struct char_data *ch, char *name, int *number, int where);
struct char_data *get_char_room_vis(struct char_data *ch, char *name, int *number);
struct char_data *get_char_world_vis(struct char_data *ch, char *name, int *number);

struct obj_data *get_obj_in_list_num(int num, struct obj_data *list);
struct obj_data *get_obj_num(obj_rnum nr);
struct obj_data *get_obj_in_list_vis(struct char_data *ch, char *name, int *number, struct obj_data *list);
struct obj_data *get_obj_vis(struct char_data *ch, char *name, int *num);
struct obj_data *get_obj_in_equip_vis(struct char_data *ch, char *arg, int *number, struct obj_data *equipment[]);
int              get_obj_pos_in_equip_vis(struct char_data *ch, char *arg, int *num, struct obj_data *equipment[]);

/* find all dots */
int	find_all_dots(char *arg);

#define FIND_INDIV	0
#define FIND_ALL	1
#define FIND_ALLDOT	2

/* group */
struct group_data * create_group(struct char_data * leader);
void free_group(struct group_data * group);
void leave_group(struct char_data *ch);
void join_group(struct char_data *ch, struct group_data *group);

/* Generic Find */
int	generic_find(char *arg, bitvector_t bitvector, struct char_data *ch,
		struct char_data **tar_ch, struct obj_data **tar_obj);

#define FIND_CHAR_ROOM     (1 << 0)
#define FIND_CHAR_WORLD    (1 << 1)
#define FIND_OBJ_INV       (1 << 2)
#define FIND_OBJ_ROOM      (1 << 3)
#define FIND_OBJ_WORLD     (1 << 4)
#define FIND_OBJ_EQUIP     (1 << 5)


/* prototypes from mobact.c */
void forget(struct char_data *ch, struct char_data *victim);
void remember(struct char_data *ch, struct char_data *victim);
void mobile_activity(void);
void clearMemory(struct char_data *ch);


/* For new last command: */
#define LAST_FILE LIB_ETC       "last"

#define LAST_CONNECT            0
#define LAST_ENTER_GAME         1
#define LAST_RECONNECT          2
#define LAST_TAKEOVER           3
#define LAST_QUIT               4
#define LAST_IDLEOUT            5
#define LAST_DISCONNECT         6
#define LAST_SHUTDOWN           7
#define LAST_REBOOT             8
#define LAST_CRASH              9
#define LAST_PLAYING            10

/* A raw binary record in LAST_FILE (written and read with fwrite/fread in
 * act.wizard.c). Its layout depends on time_t size, padding
 * and ABI, so the file is not portable between builds; a record must be
 * written from a zeroed instance (memset or {0}) so padding bytes and the
 * unused tails of hostname/username never carry stack contents to disk. */
struct last_entry {
  int close_type;
  char hostname[256];
  char username[16];
  time_t time;
  time_t close_time;
  int idnum;
  int punique;
};

void add_llog_entry(struct char_data *ch, int type);
struct last_entry *find_llog_entry(int punique, long idnum);

#endif /* TBAMUD_HANDLER_H_INCLUDED */
