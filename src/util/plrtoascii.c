/* ************************************************************************
*  file:  plrtoascii.c                                     Part of tbaMUD *
*  Copyright (C) 1990, 1991 - see 'license.doc' for complete information. *
*  All Rights Reserved                                                    *
************************************************************************* */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "db.h"
#include "pfdefaults.h"


// first some stock circle 3.0 defines. Change where appropriate.
#define MAX_NAME_LENGTH		20  /* Used in char_file_u *DO*NOT*CHANGE* */

#define MAX_PWD_LENGTH		30  /* Used in char_file_u *DO*NOT*CHANGE* */
#define MAX_TITLE_LENGTH	80  /* Used in char_file_u *DO*NOT*CHANGE* */
#define HOST_LENGTH		40  /* Used in char_file_u *DO*NOT*CHANGE* */
#define MAX_TONGUE		3   /* Used in char_file_u *DO*NOT*CHANGE* */
#define MAX_SKILLS		200 /* Used in char_file_u *DO*NOT*CHANGE* */
#define MAX_AFFECT		32  /* Used in char_file_u *DO*NOT*CHANGE* */

/* Char's abilities.  Used in char_file_u *DO*NOT*CHANGE* */
struct char_ability_data_plrtoascii {
   sbyte str;
   sbyte str_add;      /* 000 - 100 if strength 18             */
   sbyte intel;
   sbyte wis;
   sbyte dex;
   sbyte con;
   sbyte cha;
};


/* Char's points.  Used in char_file_u *DO*NOT*CHANGE* */
struct char_point_data_plrtoascii {
   sh_int mana;
   sh_int max_mana;     /* Max mana for PC/NPC			   */
   sh_int hit;
   sh_int max_hit;      /* Max hit for PC/NPC                      */
   sh_int move;
   sh_int max_move;     /* Max move for PC/NPC                     */

   sh_int armor;        /* Internal -100..100, external -10..10 AC */
   int	gold;           /* Money carried                           */
   int	bank_gold;	/* Gold the char has in a bank account	   */
   int	exp;            /* The experience of the player            */

   sbyte hitroll;       /* Any bonus or penalty to the hit roll    */
   sbyte damroll;       /* Any bonus or penalty to the damage roll */
};


/* 
 * char_special_data_saved: specials which both a PC and an NPC have in
 * common, but which must be saved to the playerfile for PC's.
 *
 * WARNING:  Do not change this structure.  Doing so will ruin the
 * playerfile.  If you want to add to the playerfile, use the spares
 * in player_special_data.
 */
struct char_special_data_saved_plrtoascii {
   int	alignment;		/* +-1000 for alignments                */
   long	idnum;			/* player's idnum; -1 for mobiles	*/
   long /*bitvector_t*/ act;	/* act flag for NPC's; player flag for PC's */

   long /*bitvector_t*/	affected_by;
				/* Bitvector for spells/skills affected by */
   sh_int apply_saving_throw[5]; /* Saving throw (Bonuses)		*/
};

struct player_special_data_saved_plrtoascii {
   byte skills[MAX_SKILLS+1];	/* array of skills plus skill 0		*/
   byte PADDING0;		/* used to be spells_to_learn		*/
   bool talks[MAX_TONGUE];	/* PC s Tongues 0 for NPC		*/
   int	wimp_level;		/* Below this # of hit points, flee!	*/
   byte freeze_level;		/* Level of god who froze char, if any	*/
   sh_int invis_level;		/* level of invisibility		*/
   room_vnum load_room;		/* Which room to place char in		*/
   long /*bitvector_t*/	pref;	/* preference flags for PC's.		*/
   ubyte bad_pws;		/* number of bad password attemps	*/
   sbyte conditions[3];         /* Drunk, full, thirsty			*/

   /* spares below for future expansion.  You can change the names from
      'sparen' to something meaningful, but don't change the order.  */

   ubyte spare0;
   ubyte spare1;
   ubyte spare2;
   ubyte spare3;
   ubyte spare4;
   ubyte page_length;
   int spells_to_learn;		/* How many can you learn yet this level*/
   int olc_zone;
   int spare8;
   int spare9;
   int spare10;
   int spare11;
   int spare12;
   int spare13;
   int spare14;
   int spare15;
   int spare16;
   long	spare17;
   long	spare18;
   long	spare19;
   long	spare20;
   long	spare21;
};

struct affected_type_plrtoascii {
   sh_int type;          /* The type of spell that caused this      */
   sh_int duration;      /* For how long its effects will last      */
   sbyte modifier;       /* This is added to apropriate ability     */
   byte location;        /* Tells which ability to change(APPLY_XXX)*/
   long /*bitvector_t*/	bitvector; /* Tells which bits to set (AFF_XXX) */

   struct affected_type_plrtoascii *next;
};

/* ==================== File Structure for Player ======================= */
/*             BEWARE: Changing it will ruin the playerfile		  */
/*
 * plrtoascii <playerfile>
 *
 * Converts a CircleMUD 3.0 binary playerfile (an array of these structs,
 * written raw by fwrite) into ASCII pfiles under plrfiles/<A-E..>/ and a
 * new plrfiles/index.  Run it from lib/ with the server stopped; it
 * OVERWRITES plrfiles/index and any existing <name>.plr, and opens the
 * input read-write ("r+") although it never writes to it.
 *
 * ABI: the layout is the source host's in-memory layout -- the widths of
 * time_t, long and int and the compiler's padding.  The tool must be built
 * with the same ABI as the server that wrote the playerfile (a file from a
 * 32-bit server cannot be read by a 64-bit build of this tool).  A file
 * whose size is not a whole number of records is refused ("plrfile-size",
 * which prints the record size this build expects); a matching size does
 * not prove the ABI matches, so check the converted output.
 *
 * Every text field is fixed-width and may be unterminated in a corrupt
 * file; it is printed bounded to its slot, and the name is validated
 * before it is used as a path.
 */
struct char_file_u_plrtoascii {
   /* char_player_data */
   char	name[MAX_NAME_LENGTH+1];
   char	description[PLR_DESC_LENGTH];
   char	title[MAX_TITLE_LENGTH+1];
   byte sex;
   byte chclass;
   byte level;
   sh_int hometown;
   time_t birth;   /* Time of birth of character     */
   int	played;    /* Number of secs played in total */
   ubyte weight;
   ubyte height;

   char	pwd[MAX_PWD_LENGTH+1];    /* character's password */

   struct char_special_data_saved_plrtoascii char_specials_saved;
   struct player_special_data_saved_plrtoascii player_specials_saved;
   struct char_ability_data_plrtoascii abilities;
   struct char_point_data_plrtoascii points;
   struct affected_type_plrtoascii affected[MAX_AFFECT];

   time_t last_logon;		/* Time (in secs) of last logon */
   char host[HOST_LENGTH+1];	/* host of last logon */
};
/* ====================================================================== */

int sprintascii(char *out, bitvector_t bits);
int plr_filename(const char *name_field, size_t name_field_size,
                 char *filename, size_t filename_size);

/* Length of a fixed-width char field read raw from the binary playerfile.
 * The old server NUL-terminated these fields, but a corrupt or crafted file
 * need not, so never run strlen() past the field: returns field_size when
 * there is no NUL inside it. */
static size_t plr_field_len(const char *field, size_t field_size)
{
  const char *nul = memchr(field, '\0', field_size);

  return nul ? (size_t)(nul - field) : field_size;
}

/* Warn about a fixed-width text field that fills its slot with no NUL.  The
 * caller prints it bounded to the field (%.*s), so the converted pfile keeps
 * the field's bytes but the operator learns the record was not well formed. */
static void plr_check_terminated(const char *filename, long record,
                                 long idnum, const char *tag,
                                 const char *field, size_t field_size)
{
  if (memchr(field, '\0', field_size) == NULL)
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_WARN, TB_ERR_PERSISTENCE,
                            "plr-field-unterminated",
                            "file=%s record=%ld idnum=%ld field=%s size=%zu: "
                            "written bounded to the field", filename, record,
                            idnum, tag, field_size);
}

/* FIXED_STR_ARGS(field): the "%.*s" argument pair for a fixed-width char
 * array from a binary record, which need not hold a NUL.  The printed text
 * stops at the first NUL or at the end of the array, never past it.  field
 * must be an array (sizeof of a pointer would bound it to 8 bytes) and is
 * evaluated twice, so pass a plain struct member. */
#define FIXED_STR_ARGS(field) \
  (int)plr_field_len((field), sizeof(field)), (field)

/* Converts every record of the binary playerfile.  Returns the number of
 * records that were reported and skipped (0 when all converted). */
int convert(char *filename)
{
  FILE *fl, *outfile, *index_file;
  struct char_file_u_plrtoascii player;
  char index_name[40], outname[128], bits[127] = "0";
  int i, skipped = 0;
  long record = 0;
  struct char_special_data_saved_plrtoascii *csds;
  struct player_special_data_saved_plrtoascii *psds;
  struct char_ability_data_plrtoascii *cad;
  struct char_point_data_plrtoascii *cpd;
  struct affected_type_plrtoascii *aff;

  if (!(fl = fopen(filename, "r+"))) {
    TB_REPORT_ERRNO("fopen", filename);
    exit(1);
  }
  /* The playerfile is an array of raw structs written by the old server, so
   * its size must be a whole number of records of the layout compiled here.
   * Any remainder means a truncated file or a writer with a different ABI
   * (time_t/long width, padding): every field after the first mismatch would
   * be garbage, so refuse the whole file before writing anything. */
  {
    struct stat st;

    if (fstat(fileno(fl), &st) != 0) {
      TB_REPORT_ERRNO("fstat", filename);
      exit(1);
    }
    if (st.st_size <= 0 ||
        (unsigned long long)st.st_size % sizeof(struct char_file_u_plrtoascii) != 0) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_FATAL, TB_ERR_PERSISTENCE, "plrfile-size",
                              "file=%s size=%lld record_size=%zu: not a whole "
                              "number of records (truncated file or a "
                              "playerfile written with a different ABI)",
                              filename, (long long)st.st_size,
                              sizeof(struct char_file_u_plrtoascii));
      exit(1);
    }
  }
  tb_snprintf(index_name, sizeof(index_name), "%s%s", LIB_PLRFILES, INDEX_FILE);
  if (!(index_file = fopen(index_name, "w"))) {
    TB_REPORT_ERRNO("fopen", index_name);
    exit(1);
  }
  for (;;) {
    if (fread(&player, sizeof(struct char_file_u_plrtoascii), 1, fl) != 1) {
      if (feof(fl)) {
        tb_fclose(fl);
        tb_fclose(index_file);
        return (skipped);
      }
      TB_REPORT_ERRNO("fread", filename);
      tb_fclose(fl);
      tb_fclose(index_file);
      exit(1);
    }

    record++;

    /* player.name is the raw 21-byte field from the file.  plr_filename()
     * validates it (terminated, non-empty, letters only) because it becomes
     * a path: a name such as "../../x" would otherwise write outside
     * plrfiles/.  The reason is reported there; the record is named here. */
    if (!plr_filename(player.name, sizeof(player.name), outname, sizeof(outname))) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "plr-name",
                              "file=%s record=%ld offset=%lld idnum=%ld: "
                              "record skipped", filename, record,
                              (long long)(record - 1) *
                              (long long)sizeof(player),
                              player.char_specials_saved.idnum);
      skipped++;
      continue;
    }
    printf("writing: %s\n", outname);

    if (!(outfile = fopen(outname, "w"))) {
      TB_REPORT_ERRNO("fopen", outname);
      exit(1);
    }

/* char_file_u */
    /* Every text field is printed bounded to its slot: the file is raw
     * structs and an unterminated field would otherwise be read on into the
     * following fields (or past the record). */
    plr_check_terminated(filename, record, player.char_specials_saved.idnum,
                         "Pass", player.pwd, sizeof(player.pwd));
    plr_check_terminated(filename, record, player.char_specials_saved.idnum,
                         "Titl", player.title, sizeof(player.title));
    plr_check_terminated(filename, record, player.char_specials_saved.idnum,
                         "Desc", player.description, sizeof(player.description));
    plr_check_terminated(filename, record, player.char_specials_saved.idnum,
                         "Host", player.host, sizeof(player.host));
    tb_fprintf(outfile, "Name: %s\n", player.name);	/* validated above */
    tb_fprintf(outfile, "Pass: %.*s\n", FIXED_STR_ARGS(player.pwd));
    tb_fprintf(outfile, "Titl: %.*s\n", FIXED_STR_ARGS(player.title));
    if (*player.description)
      tb_fprintf(outfile, "Desc:\n%.*s~\n", FIXED_STR_ARGS(player.description));
    if (player.sex != PFDEF_SEX)
      tb_fprintf(outfile, "Sex : %d\n", (int)player.sex); 
    if (player.chclass != PFDEF_CLASS)
      tb_fprintf(outfile, "Clas: %d\n", (int)player.chclass); 
    if (player.level != PFDEF_LEVEL)
      tb_fprintf(outfile, "Levl: %d\n", (int)player.level); 
    tb_fprintf(outfile, "Brth: %d\n", (int)player.birth);
    tb_fprintf(outfile, "Plyd: %d\n", (int)player.played);
    tb_fprintf(outfile, "Last: %d\n", (int)player.last_logon);
    tb_fprintf(outfile, "Host: %.*s\n", FIXED_STR_ARGS(player.host));
    if (player.height != PFDEF_HEIGHT)
      tb_fprintf(outfile, "Hite: %d\n", (int)player.height);
    if (player.weight != PFDEF_WEIGHT)
      tb_fprintf(outfile, "Wate: %d\n", (int)player.weight);

/* char_special_data_saved */
    csds = &(player.char_specials_saved);
    if (csds->alignment != PFDEF_ALIGNMENT)
      tb_fprintf(outfile, "Alin: %d\n", csds->alignment);
    tb_fprintf(outfile, "Id  : %d\n", (int)csds->idnum);
    if (csds->act != PFDEF_PLRFLAGS)
      tb_fprintf(outfile, "Act : %d\n", (int)csds->act);
    if (csds->affected_by != PFDEF_AFFFLAGS) {
      sprintascii(bits, csds->affected_by);
      tb_fprintf(outfile, "Aff : %s\n", bits);
    }
    if (csds->apply_saving_throw[0] != PFDEF_SAVETHROW)
      tb_fprintf(outfile, "Thr1: %d\n", csds->apply_saving_throw[0]);
    if (csds->apply_saving_throw[1] != PFDEF_SAVETHROW)
      tb_fprintf(outfile, "Thr2: %d\n", csds->apply_saving_throw[1]);
    if (csds->apply_saving_throw[2] != PFDEF_SAVETHROW)
      tb_fprintf(outfile, "Thr3: %d\n", csds->apply_saving_throw[2]);
    if (csds->apply_saving_throw[3] != PFDEF_SAVETHROW)
      tb_fprintf(outfile, "Thr4: %d\n", csds->apply_saving_throw[3]);
    if (csds->apply_saving_throw[4] != PFDEF_SAVETHROW)
      tb_fprintf(outfile, "Thr5: %d\n", csds->apply_saving_throw[4]);

/* player_special_data_saved */
    psds = &(player.player_specials_saved);
    if (player.level < LVL_IMMORT) {
      tb_fprintf(outfile, "Skil:\n");
      for (i = 1; i <= MAX_SKILLS; i++) {
	if (psds->skills[i])
	  tb_fprintf(outfile, "%d %d\n", i, (int)psds->skills[i]);
      }
      tb_fprintf(outfile, "0 0\n");
    }
    if (psds->wimp_level != PFDEF_WIMPLEV)
      tb_fprintf(outfile, "Wimp: %d\n", psds->wimp_level);
    if (psds->freeze_level != PFDEF_FREEZELEV)
      tb_fprintf(outfile, "Frez: %d\n", (int)psds->freeze_level);
    if (psds->invis_level != PFDEF_INVISLEV)
      tb_fprintf(outfile, "Invs: %d\n", (int)psds->invis_level);
    if (psds->load_room != PFDEF_LOADROOM)
      tb_fprintf(outfile, "Room: %d\n", (int)psds->load_room);
    if (psds->pref != PFDEF_PREFFLAGS) {
      sprintascii(bits, psds->pref);
      tb_fprintf(outfile, "Pref: %s\n", bits);
    }
    if (psds->conditions[HUNGER] && player.level < LVL_IMMORT &&
	psds->conditions[HUNGER] != PFDEF_HUNGER)
      tb_fprintf(outfile, "Hung: %d\n", (int)psds->conditions[0]);
    if (psds->conditions[THIRST] && player.level < LVL_IMMORT &&
	psds->conditions[THIRST] != PFDEF_THIRST)
      tb_fprintf(outfile, "Thir: %d\n", (int)psds->conditions[1]);
    if (psds->conditions[2] && player.level < LVL_IMMORT &&
	psds->conditions[DRUNK] != PFDEF_DRUNK)
      tb_fprintf(outfile, "Drnk: %d\n", (int)psds->conditions[2]);
    if (psds->spells_to_learn != PFDEF_PRACTICES)
      tb_fprintf(outfile, "Lern: %d\n", (int)psds->spells_to_learn);

/* char_ability_data */
    cad = &(player.abilities);
    if (cad->str != PFDEF_STR || cad->str_add != PFDEF_STRADD)
      tb_fprintf(outfile, "Str : %d/%d\n", cad->str, cad->str_add);
    if (cad->intel != PFDEF_INT)
      tb_fprintf(outfile, "Int : %d\n", cad->intel);
    if (cad->wis != PFDEF_WIS)
      tb_fprintf(outfile, "Wis : %d\n", cad->wis);
    if (cad->dex != PFDEF_DEX)
      tb_fprintf(outfile, "Dex : %d\n", cad->dex);
    if (cad->con != PFDEF_CON)
      tb_fprintf(outfile, "Con : %d\n", cad->con);
    if (cad->cha != PFDEF_CHA)
      tb_fprintf(outfile, "Cha : %d\n", cad->cha);

/* char_point_data */
    cpd = &(player.points);
    if (cpd->hit != PFDEF_HIT || cpd->max_hit != PFDEF_MAXHIT)
      tb_fprintf(outfile, "Hit : %d/%d\n", cpd->hit, cpd->max_hit);
    if (cpd->mana != PFDEF_MANA || cpd->max_mana != PFDEF_MAXMANA)
      tb_fprintf(outfile, "Mana: %d/%d\n", cpd->mana, cpd->max_mana);
    if (cpd->move != PFDEF_MOVE || cpd->max_move != PFDEF_MAXMOVE)
      tb_fprintf(outfile, "Move: %d/%d\n", cpd->move, cpd->max_move);
    if (cpd->armor != PFDEF_AC)
      tb_fprintf(outfile, "Ac  : %d\n", cpd->armor);
    if (cpd->gold != PFDEF_GOLD)
      tb_fprintf(outfile, "Gold: %d\n", cpd->gold);
    if (cpd->bank_gold != PFDEF_BANK)
      tb_fprintf(outfile, "Bank: %d\n", cpd->bank_gold);
    if (cpd->exp != PFDEF_EXP)
      tb_fprintf(outfile, "Exp : %d\n", cpd->exp);
    if (cpd->hitroll != PFDEF_HITROLL)
      tb_fprintf(outfile, "Hrol: %d\n", cpd->hitroll);
    if (cpd->damroll != PFDEF_DAMROLL)
      tb_fprintf(outfile, "Drol: %d\n", cpd->damroll);

/* affected_type */
    tb_fprintf(outfile, "Affs:\n");
    for (i = 0; i < MAX_AFFECT; i++) {
      aff = &(player.affected[i]);
      if (aff->type)
	tb_fprintf(outfile, "%d %d %d %d %d\n", aff->type, aff->duration,
	  aff->modifier, aff->location, (int)aff->bitvector);
    }
    tb_fprintf(outfile, "0 0 0 0 0\n");

    tb_fclose(outfile);

    /* Index record in the format build_player_index() (players.c) parses:
     * "id name level flags last", name lowercased, flags as PINDEX_* ascii
     * letters.  Written only after the record converted, so the index never
     * names a pfile that does not exist.  The name was validated above
     * (letters only, terminated) by plr_filename(). */
    {
      char index_name_text[MAX_NAME_LENGTH + 1], index_bits[127];
      bitvector_t index_flags = 0;
      size_t n;

      for (n = 0; player.name[n] != '\0' && n < sizeof(index_name_text) - 1; n++)
        index_name_text[n] = LOWER(player.name[n]);
      index_name_text[n] = '\0';

      if (csds->act & (1L << PLR_DELETED))
        index_flags |= PINDEX_DELETED;
      if (csds->act & (1L << PLR_NODELETE))
        index_flags |= PINDEX_NODELETE;
      if (csds->act & (1L << PLR_NOWIZLIST))
        index_flags |= PINDEX_NOWIZLIST;
      sprintascii(index_bits, index_flags);

      tb_fprintf(index_file, "%ld %s %d %s %ld\n",
                 player.char_specials_saved.idnum, index_name_text,
                 (int)player.level, index_bits, (long)player.last_logon);
    }
  }
}


int main(int argc, char **argv)
{
  if (argc != 2) {
    printf("Usage: %s playerfile-name\n", argv[0]);
    return 1;
  }
  if (convert(argv[1]) > 0) {
    /* Each skipped record was reported where it was rejected. */
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                            "plrfile-incomplete",
                            "file=%s: one or more records were skipped",
                            argv[1]);
    return 1;
  }
  return 0;
}


int sprintascii(char *out, bitvector_t bits)
{
  int i, j = 0;
  /* 32 bits, don't just add letters to try to get more unless your bitvector_t is also as large. */
  char *flags = "abcdefghijklmnopqrstuvwxyzABCDEF";

  for (i = 0; flags[i] != '\0'; i++)
    if (bits & ((bitvector_t)1 << i))
      out[j++] = flags[i];

  if (j == 0) /* Didn't write anything. */
    out[j++] = '0';

  /* NUL terminate the output string. */
  out[j++] = '\0';
  return j;
}


/*
 * Build "plrfiles/<A-E..>/<name>.plr" for one record.
 *
 * name_field is the raw fixed-width name slot from the binary file (not a C
 * string: it may be unterminated or hold any bytes).  The name becomes a path
 * component, so it must be NUL-terminated inside the slot, non-empty and made
 * of ASCII letters only, as the server's own name rules require; anything
 * else ("../x", "a/b", control bytes) is reported as "plr-name" and rejected.
 * filename_size is the caller's real buffer size; a path that does not fit is
 * reported as "plr-path-truncated" and rejected.  Returns 1 on success.
 */
int plr_filename(const char *name_field, size_t name_field_size,
                 char *filename, size_t filename_size)
{
  const char *middle;
  char name[MAX_NAME_LENGTH + 1];
  size_t len, i;
  int written;

  if (name_field == NULL || filename == NULL || filename_size == 0) {
    TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_INVARIANT, "plr-name",
                           "NULL name field or output buffer");
    return (0);
  }

  len = plr_field_len(name_field, name_field_size);
  if (len == name_field_size || len >= sizeof(name)) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "plr-name",
                            "name field of %zu bytes has no terminator",
                            name_field_size);
    return (0);
  }
  if (len == 0) {
    TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "plr-name",
                           "empty name");
    return (0);
  }
  for (i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)name_field[i];

    if (ch > 127 || !isalpha(ch)) {
      TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "plr-name",
                              "byte %zu of the name is 0x%02x, not a letter",
                              i, (unsigned int)ch);
      return (0);
    }
    name[i] = (char)tolower(ch);
  }
  name[len] = '\0';

  switch (*name) {
  case 'a':  case 'b':  case 'c':  case 'd':  case 'e':
    middle = "A-E";
    break;
  case 'f':  case 'g':  case 'h':  case 'i':  case 'j':
    middle = "F-J";
    break;
  case 'k':  case 'l':  case 'm':  case 'n':  case 'o':
    middle = "K-O";
    break;
  case 'p':  case 'q':  case 'r':  case 's':  case 't':
    middle = "P-T";
    break;
  default:	/* 'u'..'z': the name was checked to be letters only */
    middle = "U-Z";
    break;
  }

  written = snprintf(filename, filename_size, "%s%s"SLASH"%s.%s",
                     LIB_PLRFILES, middle, name, SUF_PLR);
  if (written < 0 || (size_t)written >= filename_size) {
    TB_REPORT_DOMAIN_ERRORF(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                            "plr-path-truncated", "need=%d have=%zu",
                            written, filename_size);
    return (0);
  }
  return (1);
}
