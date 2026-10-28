/* ************************************************************************
*  File: wld2html.c                                        Part of tbaMUD *
*  Usage: Convert a DikuMUD .wld file into a series of .html files        *
*                                                                         *
*  This program is in the public domain.                                  *
*  Written (QUICKLY AND DIRTILY) by Jeremy Elson                          *
*  Based on the Circle 3.0 syntax checker program                         *
************************************************************************ */

/**
 * @file wld2html.c
 * Offline tool: convert tbaMUD/CircleMUD room files (.wld) into one static
 * HTML page per room.
 *
 * Usage: wld2html <file.wld> [<file.wld> ...]
 *
 * Input (who controls it): the .wld files named on the command line, i.e.
 * builder data.  It is treated as untrusted for memory safety: every
 * length, index and number read from it is checked, and any malformed
 * input stops the run with a structured TBERR diagnostic naming the input
 * file, line and room vnum (WLD_PARSE_ERROR).  Supported per room:
 * "#<vnum>", name~, description~, "<zone> <flags> <sector>" (one flag word,
 * letters a..F or a decimal number), D<0..9> exits, E extra descriptions,
 * and S.  "T" trigger lines are skipped, "*" lines are comments, "$" ends
 * a file, and "#99999" is honoured as the old end-of-world sentinel
 * (reported).  Lines are limited to WLD_MAX_LINE - 1 bytes (255 for record
 * lines), strings to MAX_STRING_LENGTH - 1 bytes.
 *
 * Output: "<vnum>.html" in the current directory for every room, silently
 * overwriting files of that name.  Builder text is HTML-escaped.  Progress
 * goes to stderr.
 *
 * Exit status: 0 when every page was written; 1 on bad usage, any parse
 * error, an unopenable input or output file, or a failed write.
 *
 * Memory: world[] and every string hang off globals and are deliberately
 * not freed before exit; Valgrind reports them as "still reachable", not
 * as leaks.
 */

#define log(msg) tb_fprintf(stderr, "%s\n", (msg))

#include "conf.h"
#include "sysdep.h"

#define NOWHERE    (-1)		/* nil reference for room-database         */

/* The cardinal directions: used as index to room_data.dir_option[] */
#define NORTH          0
#define EAST           1
#define SOUTH          2
#define WEST           3
#define UP             4
#define DOWN           5
#define NORTHWEST      6
#define NORTHEAST      7
#define SOUTHEAST      8
#define SOUTHWEST      9

#define NUM_OF_DIRS    10

/* True when d may index room_data.dir_option[].  Evaluates d twice. */
#define VALID_DIR(d)   ((d) >= 0 && (d) < NUM_OF_DIRS)

/*
 * Exit num of world[] record room (an index, not a vnum).  Same name and
 * meaning as W_EXIT in src/utils.h, which this standalone tool cannot
 * include.  The caller must have checked room < rec_count and
 * VALID_DIR(num); each argument is evaluated once.
 */
#define W_EXIT(room, num)     (world[(room)].dir_option[(num)])

#define CREATE(result, type, number)  do {\
	if (!((result) = (type *) calloc ((number), sizeof(type))))\
		{ TB_REPORT_ERRNO("malloc", "wld2html allocation"); abort(); } } while(0)

/* Exit info: used in room_data.dir_option.exit_info */
#define EX_ISDOOR		(1 << 0)	/* Exit is a door          */
#define EX_CLOSED		(1 << 1)	/* The door is closed      */
#define EX_LOCKED		(1 << 2)	/* The door is locked      */
#define EX_PICKPROOF		(1 << 3)	/* Lock can't be picked    */

#define MAX_STRING_LENGTH	8192
/* Longest physical input line (with its terminator) any reader accepts. */
#define WLD_MAX_LINE		512
/* Width of one room flag word ('a'..'z' = bits 0..25, 'A'..'F' = 26..31). */
#define WLD_FLAG_BITS		32
/* Old Diku "#99999" end-of-world marker. */
#define WLD_SENTINEL_VNUM	99999

/*
 * Only the types this tool uses.  It must not typedef bool: that is a
 * keyword in C23 (GCC 15's default when src/util/Makefile.in passes no
 * -std), where "typedef char bool;" is a hard error.
 */
typedef signed short int sh_int;

typedef int room_num;
typedef int obj_num;


char buf[MAX_STRING_LENGTH];
char buf1[MAX_STRING_LENGTH];
char buf2[MAX_STRING_LENGTH];
char arg[MAX_STRING_LENGTH];

int get_line(FILE * fl, char *buf, size_t buflen);

/*
 * Parse context for diagnostics.  The input .wld files are builder data
 * named on the command line; when one is malformed the operator needs the
 * file, the physical line and the room vnum to fix it, so every loader
 * diagnostic goes through WLD_REPORT / WLD_PARSE_ERROR, which prefix that
 * context.  wld_cur_line counts lines consumed by get_line() and
 * fread_string(); wld_cur_vnum is the room being parsed.
 */
static const char *wld_cur_file = "(no file)";
static long wld_cur_line = 0;
static int wld_cur_vnum = 0;
static int wld_have_vnum = 0;	/* 0 until the first '#<vnum>' is parsed */
static int wld_write_failed = 0;	/* set when any HTML write fails     */

static void wld_format_context(char *dst, size_t dst_size, const char *detail)
{
  if (wld_have_vnum)
    tb_snprintf(dst, dst_size, "file %s line %ld room #%d: %s",
                wld_cur_file, wld_cur_line, wld_cur_vnum, detail);
  else
    tb_snprintf(dst, dst_size, "file %s line %ld (before first room): %s",
                wld_cur_file, wld_cur_line, detail);
}

/*
 * WLD_REPORT(severity, category, op, fmt, ...) emits a structured diagnostic
 * carrying the caller's __FILE__/__LINE__/__func__ plus the input file, line
 * and vnum.  WLD_PARSE_ERROR(op, fmt, ...) does the same at ERROR/PARSER
 * severity and then exits with status 1, because a malformed world file is
 * never converted partially.  Both evaluate their arguments once.
 */
#define WLD_REPORT(severity, category, op, ...) do { \
  char wld_detail_[512], wld_ctx_[1024]; \
  tb_snprintf(wld_detail_, sizeof(wld_detail_), __VA_ARGS__); \
  wld_format_context(wld_ctx_, sizeof(wld_ctx_), wld_detail_); \
  TB_REPORT_DOMAIN_ERROR((severity), (category), (op), wld_ctx_); \
} while (0)

#define WLD_PARSE_ERROR(op, ...) do { \
  WLD_REPORT(TB_SEV_ERROR, TB_ERR_PARSER, (op), __VA_ARGS__); \
  exit(1); \
} while (0)

/**
 * Read one whole physical line of builder input.
 *
 * Stores the line in dst without its trailing "\n" (or "\r\n") and always
 * NUL-terminates it.  Unlike a bare fgets(), a line never comes back in
 * pieces: a line that does not fit in dst_size - 1 bytes, or that contains
 * a NUL byte (which would make strlen() disagree with what was read), is
 * reported (wld-line-too-long / wld-line-nul) and the tool exits 1.  The
 * last line of a file does not need a newline.
 *
 * @param fl        Input stream (a .wld file named on the command line).
 * @param dst       Destination buffer.
 * @param dst_size  Size of dst in bytes; must be at least 2.
 * @param what      What is being read, for diagnostics.
 * @return 1 when a line was read, 0 at end of file with nothing read.
 */
static int wld_read_line(FILE *fl, char *dst, size_t dst_size, const char *what)
{
  size_t len = 0;
  int ch;

  wld_cur_line++;	/* diagnostics below name the line being read */
  while ((ch = getc(fl)) != EOF && ch != '\n') {
    if (ch == '\0')
      WLD_PARSE_ERROR("wld-line-nul", "%s line contains a NUL byte", what);
    if (len + 1 >= dst_size)
      WLD_PARSE_ERROR("wld-line-too-long", "%s line is longer than %lu bytes",
                      what, (unsigned long)(dst_size - 1));
    dst[len++] = (char)ch;
  }

  if (ch == EOF) {
    if (ferror(fl)) {
      int read_errno = errno;

      WLD_PARSE_ERROR("wld-read", "read error in the %s: %s", what,
                      strerror(read_errno));
    }
    if (len == 0) {
      wld_cur_line--;	/* nothing was read: no line consumed */
      dst[0] = '\0';
      return 0;
    }
  }

  if (len > 0 && dst[len - 1] == '\r')
    len--;
  dst[len] = '\0';
  return 1;
}

static int parse_int_token(char **cursor, int *out)
{
  char *p, *end;
  long value;

  if (cursor == NULL || *cursor == NULL || out == NULL)
    return 0;

  p = *cursor;
  while (tb_ascii_space((unsigned char)*p))
    p++;

  errno = 0;
  value = strtol(p, &end, 10);
  if (p == end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    return 0;

  *out = (int)value;
  *cursor = end;
  return 1;
}

static int parse_word_token(char **cursor, char *out, size_t out_size)
{
  char *p;
  size_t len = 0;

  if (cursor == NULL || *cursor == NULL || out == NULL || out_size == 0)
    return 0;

  p = *cursor;
  while (tb_ascii_space((unsigned char)*p))
    p++;

  while (p[len] != '\0' && !tb_ascii_space((unsigned char)p[len]))
    len++;

  if (len == 0 || len >= out_size)
    return 0;

  memcpy(out, p, len);
  out[len] = '\0';
  *cursor = p + len;
  return 1;
}

/* room-related structures *********************************************** */


struct room_direction_data {
  char *general_description;	/* When look DIR.                        */

  char *keyword;		/* for open/close                        */

  sh_int exit_info;		/* Exit info                             */
  obj_num key;			/* Key's number (-1 for no key)          */
  room_num to_room;		/* Where direction leads (NOWHERE)       */
};

struct extra_descr_data {
  char *keyword;		/* Keyword in look/examine          */
  char *description;		/* What to see                      */
  struct extra_descr_data *next;	/* Next in list                     */
};

/* ================== Memory Structure for room ======================= */
struct room_data {
  room_num number;		/* Rooms number (vnum)                */
  sh_int zone;			/* Room zone (for resetting)          */
  int sector_type;		/* sector type (move/hide)            */
  char *name;			/* Rooms name 'You are ...'           */
  char *description;		/* Shown when entered                 */
  struct extra_descr_data *ex_description;	/* for examine/look       */
  struct room_direction_data *dir_option[NUM_OF_DIRS];	/* Directions */
  unsigned long room_flags;	/* DEATH,DARK ... (32 bits used)      */

  signed char light;		/* Number of lightsources in room     */
};

/* ====================================================================== */


/**************************************************************************
*  declarations of most of the 'global' variables                         *
************************************************************************ */

struct room_data *world = NULL;	/* array of rooms                */
int top_of_world = 0;		/* ref to top element of world   */
int rec_count = 0;



/* local functions */
char *fread_string(FILE * fl, const char *error);
void setup_dir(FILE * fl, int room, int dir);
void index_boot(int cnt, char **name);
void discrete_load(FILE * fl);
void parse_room(FILE * fl, int virtual_nr);
void write_output(void);


char *dir_names[] =
{"North", "East", "South", "West", "Up", "Down","North West","North East","South East","South West"};


/*************************************************************************
*  routines for booting the system                                       *
*********************************************************************** */

/* body of the booting system */
int main(int argc, char **argv)
{
  if (argc < 2) {
    tb_fprintf(stderr, "Usage: %s <world-file-name(s)>\n", argv[0]);
    exit(1);
  }

  index_boot(argc,argv);

  log("Writing output.");
  write_output();
  log("Done.");

  /* A failed HTML write was already reported where it happened. */
  return (wld_write_failed ? 1 : 0);
}

/* Since the world is loaded into memory by index
 * and not room number, we need to search through
 * all rooms and return the correct one. This is
 * used to generate door information (ie: the name)
 */
struct room_data* findRoom(int nr)
{
  int i;

  /*
   * Skip unused slots (name == NULL, see write_output): they are zeroed,
   * so an exit to room 0 used to match one and print its NULL name.
   */
  for (i=0;i<rec_count;i++)
    if (world[i].name != NULL && world[i].number==nr)
      return &world[i];

  return NULL;
}

/*
 * Write builder-supplied text (room names, descriptions) into a page,
 * escaping the HTML metacharacters & < > ".  The .wld text is builder data
 * and used to be copied raw, so a '<' in a description became markup in
 * the published page.  On a write error it stops; write_output()'s
 * ferror() check then reports wld-html-write once for the page.
 */
static void html_escape_fputs(FILE *fl, const char *text)
{
  const char *p;

  if (text == NULL) {
    WLD_REPORT(TB_SEV_ERROR, TB_ERR_INVARIANT, "wld-html-null-text",
               "NULL text passed for HTML output");
    return;
  }

  for (p = text; *p; p++) {
    const char *entity = NULL;

    switch (*p) {
    case '&': entity = "&amp;"; break;
    case '<': entity = "&lt;"; break;
    case '>': entity = "&gt;"; break;
    case '"': entity = "&quot;"; break;
    default: break;
    }
    if (entity != NULL) {
      if (fputs(entity, fl) == EOF)
        return;
    } else if (putc(*p, fl) == EOF)
      return;
  }
}

void write_output(void)
{
  int i;
  FILE *fl;
  char buf[128];
  register int door, found;

  for (i=0;i<rec_count;i++) {
    //print the record number, but no linefeed.
    tb_fprintf(stderr, "Record: %d ",i);
    /*
     * parse_room() gives every loaded room a non-NULL name, so NULL marks a
     * slot that was counted in pass one but never loaded (a '#' line inside
     * a string, or the #99999 sentinel).  Over-counting is expected.
     */
    if (world[i].name == NULL) {
      log("unused slot");
      continue;
    }
    tb_snprintf(buf, sizeof(buf), "Writing %d.html", world[i].number);
    log(buf);
    //for some reason, if you use %d with sprintf it rolls over like its 16bit,
    //but only if you use the buffer with fopen.
    //using %ld and casting to long solves this in case someone really wants
    //to use negative room numbers.
    tb_snprintf(buf, sizeof(buf), "%ld.html",(long)world[i].number);
    if (!(fl = fopen(buf, "w"))) {
      TB_REPORT_ERRNO("fopen", buf);
      exit(1);
    }
    tb_fprintf(fl, "<title> ");
    html_escape_fputs(fl, world[i].name);
    tb_fprintf(fl, " </title>\n<h1> ");
    html_escape_fputs(fl, world[i].name);
    tb_fprintf(fl, " </h1>\n");
    tb_fprintf(fl, "<pre>\n");
    html_escape_fputs(fl, world[i].description);
    tb_fprintf(fl, "</pre>\n");
    tb_fprintf(fl, "<P> Exits: <P> \n");

    found = 0;
    for (door = 0; door < NUM_OF_DIRS; door++)
      if (W_EXIT(i, door) &&
          W_EXIT(i, door)->to_room != NOWHERE) {
        found = 1;
	//this call gets a pointer to the room referenced by the to_room for the door.
	//This fixes a lot of issues introduced with the whole 'renumbering rooms' call
	//and the binary search that didn't work well.
	struct room_data* to_room = findRoom(W_EXIT(i, door)->to_room);
        if (to_room) {
          tb_fprintf(fl, "<a href = \"%d.html\"> %s to ",
                  to_room->number,
                  dir_names[door]);
          html_escape_fputs(fl, to_room->name);
          tb_fprintf(fl, "</a> <p>\n");
        } else
          tb_fprintf(fl, "%s to missing room %d <p>\n",
                  dir_names[door], W_EXIT(i, door)->to_room);
      }
    if (!found)
      tb_fprintf(fl, "None!");
    if (ferror(fl)) {
      TB_REPORT_DOMAIN_ERROR(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                             "wld-html-write", buf);
      wld_write_failed = 1;
    }
    if (!tb_fclose_checked(fl, buf))
      wld_write_failed = 1;
  }
}  

/*
 * Count the '#'-prefixed lines in a file: an upper bound on the rooms
 * parse_room() will store from it.  The bound only holds if this pass sees
 * the same lines as the loading pass, so both use wld_read_line(), which
 * never splits a physical line (the old 128-byte fgets() here and 256-byte
 * fgets() in get_line() could disagree and under-count).  The buffer is the
 * largest line any loader reader accepts, so this pass never rejects a line
 * the loader would accept.
 */
int count_hash_records(FILE * fl)
{
  char buf[WLD_MAX_LINE];
  int count = 0;

  while (wld_read_line(fl, buf, sizeof(buf), "input")) {
    if (*buf == '#') {
      char *cursor = buf + 1;

      /* Only for diagnostic context; parse_room() validates the vnum. */
      if (parse_int_token(&cursor, &wld_cur_vnum))
        wld_have_vnum = 1;
      count++;
    }
  }

  return (count);
}



/**
 * Load every .wld file named on the command line into world[].
 *
 * Two-pass sizing contract: pass one sums count_hash_records() over all
 * files into rec_count and world[] is calloc'd with that many entries; pass
 * two parses the files in the same order.  rec_count is only an upper
 * bound (a '#' at the start of a string line or the #99999 sentinel is
 * counted but never loaded), so unused trailing slots stay zeroed with
 * name == NULL, which write_output() and findRoom() treat as "unused".
 * parse_room() refuses to store past rec_count (wld-record-capacity).
 *
 * @param cnt    argc from main(); names[0] (the program) is skipped.
 * @param names  argv from main(): input file paths, used for diagnostics.
 * Exits 1 on any open or parse failure.
 */
void index_boot(int cnt, char **names)
{
  FILE *db_file;

  //throw first entry away as that is the executable.
  for (int i=1;i<cnt;i++) {
    if (!(db_file = fopen(names[i], "r"))) {
      TB_REPORT_ERRNO("fopen", names[i]);
      exit(1);
    }
    //have to loop through files twice.
    //once to get total record count
    //second time to load them
    wld_cur_file = names[i];
    wld_cur_line = 0;
    wld_have_vnum = 0;
    rec_count += count_hash_records(db_file);
    tb_fclose(db_file);
  }
  tb_snprintf(buf, sizeof(buf), "Total records: %d\n", rec_count);
  log(buf);

  if (rec_count <= 0) {
    world = NULL;
    return;
  }

  //now that we know how many records in total
  //we can create the memory structure
  CREATE(world, struct room_data, rec_count);
  //now loop through files and load them
  for (int i=1;i<cnt;i++) {
    if (!(db_file = fopen(names[i], "r"))) {
      TB_REPORT_ERRNO("fopen", names[i]);
      exit(1);
    }
    wld_cur_file = names[i];
    wld_cur_line = 0;
    wld_have_vnum = 0;
    discrete_load(db_file);
    tb_fclose(db_file);
  }
}


void discrete_load(FILE * fl)
{
  int nr = -1, last = 0;
  char line[256];

  for (;;) {
    if (!get_line(fl, line, sizeof(line)))
      WLD_PARSE_ERROR("wld-unexpected-eof",
                      "file ended without the '$' end-of-file marker");
    if (*line == 'T') //Toss triggers. THey currently break this util.
      continue;
	  
    if (*line == '$')
      return;

    if (*line == '#') {
      last = nr;
      {
        char *cursor = line + 1;
        if (!parse_int_token(&cursor, &nr))
          WLD_PARSE_ERROR("wld-room-vnum",
                          "'#' line after room #%d has no integer vnum", last);
      }
      wld_cur_vnum = nr;
      wld_have_vnum = 1;
      /*
       * "#99999" is the old Diku end-of-world sentinel.  Honour only that
       * exact value, and say so, because everything after it in the file
       * is ignored.  Any other vnum (the old ">= 99999" test silently
       * dropped real rooms numbered 100000 and up) is converted.
       */
      if (nr == WLD_SENTINEL_VNUM) {
        WLD_REPORT(TB_SEV_WARN, TB_ERR_PARSER, "wld-sentinel",
                   "'#%d' end-of-world sentinel: rest of file ignored", nr);
        return;
      }
      parse_room(fl, nr);
    } else
      WLD_PARSE_ERROR("wld-record-start",
                      "expected '#<vnum>', 'T' or '$', got '%.40s'", line);
  }
}

/*
 * Convert a room flag word from the .wld file: either a decimal bitvector
 * or letters, where 'a'..'z' are bits 0..25 and 'A'..'Z' are bits 26..51.
 * room_flags is a 32-bit word, so a letter past 'F' (bit 31) or a number
 * above 0xFFFFFFFF cannot be stored.  The old code shifted a signed int by
 * up to 51 places (undefined behaviour) and called islower() etc. on a
 * plain char; such words are now reported (wld-flag-bit /
 * wld-flag-value) and the tool exits 1.
 */
unsigned long asciiflag_conv(const char *flag)
{
  unsigned long flags = 0;
  int is_number = 1;
  const char *p;

  for (p = flag; *p; p++) {
    unsigned char c = (unsigned char)*p;
    int bit = -1;

    if (c >= 'a' && c <= 'z')
      bit = c - 'a';
    else if (c >= 'A' && c <= 'Z')
      bit = 26 + (c - 'A');

    if (bit >= WLD_FLAG_BITS)
      WLD_PARSE_ERROR("wld-flag-bit",
                      "room flag '%c' is bit %d; room_flags holds bits 0..%d",
                      c, bit, WLD_FLAG_BITS - 1);
    if (bit >= 0)
      flags |= 1UL << bit;

    if (c < '0' || c > '9')
      is_number = 0;
  }

  if (is_number) {
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(flag, &end, 10);
    if (end == flag || *end != '\0' || errno == ERANGE ||
        value > 0xFFFFFFFFUL)
      WLD_PARSE_ERROR("wld-flag-value",
                      "numeric room flags '%.20s' do not fit in 32 bits", flag);
    flags = value;
  }

  return (flags);
}

/**
 * Parse one room record (everything after its "#<vnum>" line through "S")
 * into the next free world[] slot.
 *
 * The static room_nr is a cursor shared by all input files: it is the
 * world[] index of the next room and only ever grows.  It must stay below
 * rec_count; that is checked before every store.  A loaded room always has
 * non-NULL name and description (empty fields are reported and replaced
 * with ""), which is what distinguishes it from an unused slot.
 *
 * @param fl          Input stream positioned after the '#' line.
 * @param virtual_nr  The room's vnum from that line.
 * Exits 1 on any format error.
 */
void parse_room(FILE * fl, int virtual_nr)
{
  static int room_nr = 0, zone = 0;
  int t[10], i;
  char line[256], flags[128];
  struct extra_descr_data *new_descr;

  /*
   * room_nr is a cursor across every input file; world[] holds rec_count
   * entries, sized by count_hash_records().  If the two passes ever
   * disagree, stop before writing past the allocation.
   */
  if (room_nr < 0 || room_nr >= rec_count) {
    WLD_REPORT(TB_SEV_ERROR, TB_ERR_INVARIANT, "wld-record-capacity",
               "record %d is outside the %d records counted in pass one",
               room_nr, rec_count);
    exit(1);
  }

  world[room_nr].zone = zone;
  world[room_nr].number = virtual_nr;
  /*
   * fread_string() returns NULL for an empty "~" string.  write_output()
   * prints both fields and treats a NULL name as "slot never loaded", so an
   * empty field is reported and replaced by an empty allocated string.
   */
  world[room_nr].name = fread_string(fl, "room name");
  if (world[room_nr].name == NULL) {
    WLD_REPORT(TB_SEV_WARN, TB_ERR_PARSER, "wld-empty-field",
               "record %d has an empty room name", room_nr);
    CREATE(world[room_nr].name, char, 1);
  }
  world[room_nr].description = fread_string(fl, "room description");
  if (world[room_nr].description == NULL) {
    WLD_REPORT(TB_SEV_WARN, TB_ERR_PARSER, "wld-empty-field",
               "record %d has an empty room description", room_nr);
    CREATE(world[room_nr].description, char, 1);
  }

  if (!get_line(fl, line, sizeof(line)))
    WLD_PARSE_ERROR("wld-unexpected-eof",
                    "file ended before the '<zone> <flags> <sector>' line");
  {
    char *cursor = line;
    if (!parse_int_token(&cursor, &t[0]) ||
        !parse_word_token(&cursor, flags, sizeof(flags)) ||
        !parse_int_token(&cursor, &t[2]))
      WLD_PARSE_ERROR("wld-room-flags-line",
                      "expected '<zone> <flags> <sector>'");
  }
  /* t[0] is the zone number; ignored with the zone-file system */
  world[room_nr].room_flags = asciiflag_conv(flags);
  world[room_nr].sector_type = t[2];

  world[room_nr].light = 0;	/* Zero light sources */

  for (i = 0; i < NUM_OF_DIRS; i++)
    W_EXIT(room_nr, i) = NULL;

  world[room_nr].ex_description = NULL;

  for (;;) {
    if (!get_line(fl, line, sizeof(line)))
      WLD_PARSE_ERROR("wld-unexpected-eof",
                      "file ended inside the room (expecting D/E/S)");
    switch (*line) {
    case 'D': {
      /*
       * "D<dir>" comes straight from the builder's .wld file and is used
       * as an index into dir_option[NUM_OF_DIRS], so anything that is not
       * an integer in 0..NUM_OF_DIRS-1 is rejected here rather than
       * becoming an out-of-bounds write (or silently direction 0).
       */
      char *cursor = line + 1;
      int dir;

      if (!parse_int_token(&cursor, &dir) || !VALID_DIR(dir))
        WLD_PARSE_ERROR("wld-door-index",
                        "door line '%.40s' needs a direction 0..%d",
                        line, NUM_OF_DIRS - 1);
      setup_dir(fl, room_nr, dir);
      break;
    }
    case 'E':
      CREATE(new_descr, struct extra_descr_data, 1);
      new_descr->keyword = fread_string(fl, "extra description keyword");
      new_descr->description = fread_string(fl, "extra description text");
      new_descr->next = world[room_nr].ex_description;
      world[room_nr].ex_description = new_descr;
      break;
    case 'S':			/* end of room */
      top_of_world = room_nr++;
      return;
      break;
    default:
      WLD_PARSE_ERROR("wld-room-section",
                      "expected a D, E or S line, got '%.40s'", line);
      break;
    }
  }
}



/**
 * Read one "D<dir>" exit block (general description~, keyword~ and the
 * "<door-flag> <key> <to-room>" line) into W_EXIT(room, dir).
 *
 * Preconditions, all re-checked here: 0 <= room < rec_count,
 * VALID_DIR(dir) (parse_room() validates the builder's D<dir> first), and
 * the slot is still empty (a repeated direction is wld-duplicate-door).
 * to_room is stored as a vnum, not an rnum; write_output() resolves it with
 * findRoom() and prints "missing room N" when nothing matches.
 * Exits 1 on any violation or format error.
 */
void setup_dir(FILE * fl, int room, int dir)
{
  int t[5];
  char line[256];

  /*
   * Preconditions (checked by parse_room): room indexes world[] and dir
   * indexes dir_option[].
   */
  if (room < 0 || room >= rec_count || !VALID_DIR(dir)) {
    WLD_REPORT(TB_SEV_ERROR, TB_ERR_INVARIANT, "wld-setup-dir",
               "record %d (of %d) exit D%d outside dir_option[0..%d]",
               room, rec_count, dir, NUM_OF_DIRS - 1);
    exit(1);
  }

  /*
   * A room may define each direction once.  A second "D<dir>" block used
   * to CREATE over the first, leaking it and silently keeping whichever
   * came last.
   */
  if (W_EXIT(room, dir) != NULL)
    WLD_PARSE_ERROR("wld-duplicate-door", "exit D%d is defined twice", dir);

  tb_snprintf(buf2, sizeof(buf2), "exit D%d text", dir);

  CREATE(W_EXIT(room, dir), struct room_direction_data, 1);
  W_EXIT(room, dir)->general_description = fread_string(fl, buf2);
  W_EXIT(room, dir)->keyword = fread_string(fl, buf2);

  if (!get_line(fl, line, sizeof(line)))
    WLD_PARSE_ERROR("wld-unexpected-eof",
                    "file ended before the D%d '<door> <key> <to-room>' line", dir);
  {
    char *cursor = line;
    if (!parse_int_token(&cursor, &t[0]) ||
        !parse_int_token(&cursor, &t[1]) ||
        !parse_int_token(&cursor, &t[2]))
      WLD_PARSE_ERROR("wld-door-line",
                      "D%d expected '<door-flag> <key> <to-room>'", dir);
  }
  if (t[0] == 1)
    W_EXIT(room, dir)->exit_info = EX_ISDOOR;
  else if (t[0] == 2)
    W_EXIT(room, dir)->exit_info = EX_ISDOOR | EX_PICKPROOF;
  else
    W_EXIT(room, dir)->exit_info = 0;

  W_EXIT(room, dir)->key = t[1];
  W_EXIT(room, dir)->to_room = t[2];
}


/*************************************************************************
*  procedures for resetting, both play-time and boot-time	 	 *
*********************************************************************** */

/**
 * Read a '~'-terminated string from a .wld file.
 *
 * Lines are joined with "\r\n"; anything after the '~' on its line is
 * ignored.  Each line may hold at most sizeof(tmp) - 3 (509) bytes and the
 * whole string MAX_STRING_LENGTH - 1 bytes; longer input, a NUL byte, or
 * end of file before the '~' is reported (wld-line-too-long, wld-line-nul,
 * wld-string-too-long, wld-unexpected-eof) and the tool exits 1.
 *
 * @param fl     Input stream.
 * @param error  Name of the field being read ("room name", "exit D0
 *               text", ...), used only as diagnostic context.
 * @return A calloc'd copy owned by the caller (this tool never frees it),
 *         or NULL for an empty string ("~" alone): callers must handle NULL.
 */
char *fread_string(FILE * fl, const char *error)
{
  char buf[MAX_STRING_LENGTH], tmp[512], *rslt;
  register char *point;
  int done = 0, length = 0, templength = 0;

  *buf = '\0';

  do {
    /*
     * Read a whole line, leaving two bytes spare for the "\r\n" appended
     * below.  wld_read_line() rejects overlong lines and NUL bytes, so
     * strlen(tmp) is exactly what was read and never exceeds
     * sizeof(tmp) - 3; the old fgets() version wrote one byte past tmp
     * for a 511-byte line and one byte before it for a leading NUL.
     */
    if (!wld_read_line(fl, tmp, sizeof(tmp) - 2, error))
      WLD_PARSE_ERROR("wld-unexpected-eof",
                      "file ended inside the '~'-terminated %s", error);
    /* If there is a '~', end the string; else end the line with "\r\n". */
    if ((point = strchr(tmp, '~')) != NULL) {
      *point = '\0';
      done = 1;
    } else {
      point = tmp + strlen(tmp);
      *(point++) = '\r';
      *(point++) = '\n';
      *point = '\0';
    }

    templength = strlen(tmp);

    if (length + templength >= MAX_STRING_LENGTH) {
      WLD_PARSE_ERROR("wld-string-too-long",
                      "%s exceeds %d bytes", error, MAX_STRING_LENGTH - 1);
    } else {
      strlcat(buf, tmp, sizeof(buf));
      length += templength;
    }
  } while (!done);

  /*
   * Allocate exactly length + 1 bytes and copy exactly that many.  The copy
   * bound must be the allocation size: the old strlcpy(rslt, buf,
   * MAX_STRING_LENGTH) claimed 8192 bytes of a length+1 block, which glibc's
   * _FORTIFY_SOURCE=3 aborts on for every string, and which became a real
   * heap overflow whenever 'length' undercounted buf.
   */
  if (length > 0) {
    size_t buf_len = strlen(buf);

    if (buf_len != (size_t)length) {
      WLD_REPORT(TB_SEV_ERROR, TB_ERR_INVARIANT, "wld-fread-string-length",
                 "%s: counted %d bytes but the buffer holds %lu",
                 error, length, (unsigned long)buf_len);
      exit(1);
    }
    CREATE(rslt, char, length + 1);
    memcpy(rslt, buf, (size_t)length + 1);
  } else
    rslt = NULL;

  return (rslt);
}



/**
 * Read the next record line, skipping blank lines and '*' comment lines.
 *
 * The trailing "\n" or "\r\n" is removed.  A line that does not fit in
 * buflen - 1 bytes (including a long comment line) or contains a NUL byte
 * is reported and the tool exits 1; it is never split.  A final line
 * without a newline is returned normally.
 *
 * @param fl      Input stream.
 * @param buf     Destination, always NUL-terminated on return.
 * @param buflen  Size of buf in bytes (callers pass 256).
 * @return 1 when a line was stored, 0 at end of file.
 */
int get_line(FILE * fl, char *buf, size_t buflen)
{
  /*
   * wld_read_line() strips only a real "\n"/"\r\n" (the old code chopped
   * the last byte unconditionally), never splits a line longer than buflen
   * (it reports wld-line-too-long instead of returning the tail as a new
   * record), and returns a final line that has no newline instead of
   * treating it as end of file.
   */
  do {
    if (!wld_read_line(fl, buf, buflen, "record"))
      return (0);
  } while (*buf == '*' || !*buf);

  return (1);
}
