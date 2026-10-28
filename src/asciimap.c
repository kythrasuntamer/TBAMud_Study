/**************************************************************************
*  File: asciimap.c                                        Part of tbaMUD *
*  Usage: Generates an ASCII map of the player's surroundings.            *
*                                                                         *
*  All rights reserved.  See license for complete information.            *
*                                                                         *
*  Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "spells.h"
#include "house.h"
#include "constants.h"
#include "dg_scripts.h"
#include "asciimap.h"

/******************************************************************************
 * Begin Local (File Scope) Defines and Global Variables
 *****************************************************************************/
/* Do not blindly change these values, as many values cause the map to stop working - backup first */
#define CANVAS_HEIGHT 19
#define CANVAS_WIDTH  51
#define LEGEND_WIDTH  15

#define DEFAULT_MAP_SIZE CONFIG_MAP_SIZE

#define MAX_MAP_SIZE ((CANVAS_WIDTH - 1) / 4)
#define MAX_MAP      CANVAS_WIDTH

/* TRUE if (x, y) is a cell of map[][]. Evaluates x and y once each. */
#define VALID_MAP_COORD(x, y) ((unsigned)(x) < MAX_MAP && (unsigned)(y) < MAX_MAP)
/* Largest step MapArea takes from a cell it expands on the normal map
 * (offsets[], ±2; offsets_worldmap[] is ±1). */
#define MAP_MAX_STEP 2

#define MAX_MAP_DIR 10
#define MAX_MAP_FOLLOW 10

#define SECT_EMPTY 30 /* anything greater than num sect types */
#define SECT_STRANGE (SECT_EMPTY + 1)
#define SECT_HERE  (SECT_STRANGE + 1)

#define DOOR_NS (-1)
#define DOOR_EW (-2)
#define DOOR_UP (-3)
#define DOOR_DOWN (-4)
#define DOOR_DIAGNE (-5)
#define DOOR_DIAGNW (-6)
#define VDOOR_NS (-7)
#define VDOOR_EW (-8)
#define VDOOR_DIAGNE (-9)
#define VDOOR_DIAGNW (-10)
#define DOOR_UP_AND_NE (-11)
#define DOOR_DOWN_AND_SE (-12)
#define DOOR_NONE        (-13)
#define NUM_DOOR_TYPES 13

#define MAP_CIRCLE    0
#define MAP_RECTANGLE 1

#define MAP_NORMAL  0
#define MAP_COMPACT 1

static bool show_worldmap(struct char_data *ch);

struct map_info_type
{
  int  sector_type;
  char disp[20];
};

static struct map_info_type door_info[] =
{
  { DOOR_NONE, "   " },
  { DOOR_DOWN_AND_SE, "\tr-\tn\\ " },
  { DOOR_UP_AND_NE,   "\tr+\tn/ " },
  { VDOOR_DIAGNW,     " \tm+\tn " },
  { VDOOR_DIAGNE,     " \tm+\tn "},
  { VDOOR_EW,  " \tm+\tn " },
  { VDOOR_NS,  " \tm+\tn "},
  { DOOR_DIAGNW,      " \\ " },
  { DOOR_DIAGNE,      " / " },
  { DOOR_DOWN, "\tr-\tn  " },
  { DOOR_UP,   "\tr+\tn  " },
  { DOOR_EW,   " - " },
  { DOOR_NS,   " | " }
};

static struct map_info_type compact_door_info[] =
{
  { DOOR_NONE, " " },
  { DOOR_DOWN_AND_SE, "\tR\\\tn" },
  { DOOR_UP_AND_NE,   "\tR/\tn" },
  { VDOOR_DIAGNW,   "\tm+\tn" },
  { VDOOR_DIAGNE,   "\tm+\tn"},
  { VDOOR_EW,  " \tm+\tn " },
  { VDOOR_NS,  " \tm+\tn "},
  { DOOR_DIAGNW,"\\" },
  { DOOR_DIAGNE,"/" },
  { DOOR_DOWN, "\tr-\tn" },
  { DOOR_UP,   "\tr+\tn" },
  { DOOR_EW,   "-" },
  { DOOR_NS,   " | " }
};

/* Add new sector types below for both map_info and world_map_info     */
/* The last 3 MUST remain the same, although the symbol can be changed */
/* New sectors also need to be added to the perform_map function below */
static struct map_info_type map_info[] =
{
  { SECT_INSIDE,       "\tc[\tn.\tc]\tn" }, /* 0 */
  { SECT_CITY,         "\tc[\twC\tc]\tn" },
  { SECT_FIELD,        "\tc[\tg,\tc]\tn" },
  { SECT_FOREST,       "\tc[\tgY\tc]\tn" },
  { SECT_HILLS,        "\tc[\tMm\tc]\tn" },
  { SECT_MOUNTAIN,     "\tc[\trM\tc]\tn" }, /* 5 */
  { SECT_WATER_SWIM,   "\tc[\tc~\tc]\tn" },
  { SECT_WATER_NOSWIM, "\tc[\tb=\tc]\tn" },
  { SECT_FLYING,       "\tc[\tC^\tc]\tn" },
  { SECT_UNDERWATER,   "\tc[\tbU\tc]\tn" },
  { -1,                ""        }, /* 10 */
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        }, /* 15 */
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        }, /* 20 */
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        }, /* 25 */
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { -1,                ""        },
  { SECT_EMPTY,        "   "     }, /* 30 */
  { SECT_STRANGE,      "\tc[\tR?\tc]\tn" },
  { SECT_HERE,         "\tc[\tB!\tc]\tn"     },
};

static struct map_info_type world_map_info[] =
{
  { SECT_INSIDE,       "\tn."  }, /* 0 */
  { SECT_CITY,         "\twC"  },
  { SECT_FIELD,        "\tg,"  },
  { SECT_FOREST,       "\tgY"  },
  { SECT_HILLS,        "\tMm"  },
  { SECT_MOUNTAIN,     "\trM"  }, /* 5 */
  { SECT_WATER_SWIM,   "\tc~"  },
  { SECT_WATER_NOSWIM, "\tb="  },
  { SECT_FLYING,       "\tC^"  },
  { SECT_UNDERWATER,   "\tbU"  },
  { -1,                ""     }, /* 10 */
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     }, /* 15 */
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     }, /* 20 */
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     }, /* 25 */
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { -1,                ""     },
  { SECT_EMPTY,        " "    }, /* 30 */
  { SECT_STRANGE,      "\tR?"  },
  { SECT_HERE,         "\tB!"  },
};


/* The drawing grid, rebuilt by every map/automap. Coordinates are 0 ..
 * MAX_MAP-1 (VALID_MAP_COORD); the viewer's room is drawn at the centre
 * (MAX_MAP/2). A cell >= 0 is a sector glyph index into map_info[] /
 * world_map_info[] (0..NUM_ROOM_SECTORS-1, or SECT_EMPTY / SECT_STRANGE /
 * SECT_HERE); a cell < 0 is a door glyph, read as door_info[NUM_DOOR_TYPES
 * + v] (DOOR_NONE .. DOOR_NS). MapArea() keeps every value in those ranges
 * (map_sector()). Not reentrant; one map is drawn at a time. */
static int map[MAX_MAP][MAX_MAP];
/*
static int offsets[4][2] ={ {-2, 0},{ 0, 2},{ 2, 0},{ 0, -2} };
static int offsets_worldmap[4][2] ={ {-1, 0},{ 0, 1},{ 1, 0},{ 0, -1} };
static int door_offsets[6][2] ={ {-1, 0},{ 0, 1},{ 1, 0},{ 0, -1},{ -1, 1},{ 1, 1} };
static int door_marks[6] = { DOOR_NS, DOOR_EW, DOOR_NS, DOOR_EW, DOOR_UP, DOOR_DOWN };
*/
static int offsets[10][2] ={ {-2, 0},{ 0, 2},{ 2, 0},{ 0, -2},{0, 0},{ 0, 0},{ -2, -2},{ -2, 2},{2, 2},{ 2, -2} };
static int offsets_worldmap[10][2] ={ {-1, 0},{ 0, 1},{ 1, 0},{ 0, -1},{0, 0},{ 0, 0},{ -1, -1},{ -1, 1},{1, 1},{ 1, -1} };
static int door_offsets[10][2] ={ {-1, 0},{ 0, 1},{ 1, 0},{ 0, -1},{ -1, 1},{ 1, 1},{ -1, -1},{ -1, 1},{ 1, 1},{ 1, -1} };
static int door_marks[10] = { DOOR_NS, DOOR_EW, DOOR_NS, DOOR_EW, DOOR_UP, DOOR_DOWN, DOOR_DIAGNW, DOOR_DIAGNE, DOOR_DIAGNW, DOOR_DIAGNE};
static int vdoor_marks[4] = { VDOOR_NS, VDOOR_EW, VDOOR_NS, VDOOR_EW };
/******************************************************************************
 * End Local (File Scope) Defines and Global Variables
 *****************************************************************************/

/******************************************************************************
 * Begin Local (File Scope) Function Prototypes
 *****************************************************************************/
static void MapArea(room_rnum room, struct char_data *ch, int x, int y, int min, int max, sh_int xpos, sh_int ypos, bool worldmap);
static char *StringMap(int centre, int size);
static char *WorldMap(int centre, int size, int mapshape, int maptype );
static char *CompactStringMap(int centre, int size);
static void perform_map( struct char_data *ch, char *argument, bool worldmap );
/******************************************************************************
 * End Local (File Scope) Function Prototypes
 *****************************************************************************/


bool can_see_map(struct char_data *ch) {
  /* Is the map funcionality disabled? */
  if (CONFIG_MAP == MAP_OFF)
    return FALSE;
  else if ((CONFIG_MAP == MAP_IMM_ONLY) && (GET_LEVEL(ch) < LVL_IMMORT))
    return FALSE;

  return TRUE;
}

/* Map cell value for room's sector. Cells are read back as indexes: a value
 * >= 0 indexes map_info[]/world_map_info[] (SECT_HERE + 1 entries) and a
 * negative one door_info[NUM_DOOR_TYPES + v]. sector_type comes from the
 * world file, whose loader does not fully range-check it, so anything
 * outside 0..NUM_ROOM_SECTORS-1 is drawn as SECT_STRANGE and reported (once
 * per room in a row, since every 'look' redraws it). */
static int map_sector(room_rnum room)
{
  static room_rnum last_reported = NOWHERE;
  int sector = SECT(room);

  if (sector >= 0 && sector < NUM_ROOM_SECTORS)
    return sector;
  if (room != last_reported) {
    TB_WARN_MSG("map", "bad-sector", "world",
                "room vnum %d sector %d outside [0,%d); drawn as unknown",
                VALID_ROOM_RNUM(room) ? (int) GET_ROOM_VNUM(room) : -1,
                sector, NUM_ROOM_SECTORS);
    last_reported = room;
  }
  return SECT_STRANGE;
}

/* TRUE if exit `door` of room leads to a room MapArea can index. NOWHERE
 * is an ordinary "no destination"; anything past top_of_world is corrupt
 * world data and is reported (once per room in a row, as for sectors). */
static bool map_exit_target_ok(room_rnum room, int door,
                               const struct room_direction_data *pexit)
{
  static room_rnum last_reported = NOWHERE;

  if (pexit->to_room == NOWHERE)
    return FALSE;
  if (VALID_ROOM_RNUM(pexit->to_room))
    return TRUE;
  if (room != last_reported) {
    TB_ERROR_MSG("map", "exit-range", "world",
                 "room vnum %d exit %d leads to rnum %d outside [0,%d]",
                 (int) GET_ROOM_VNUM(room), door, (int) pexit->to_room,
                 (int) top_of_world);
    last_reported = room;
  }
  return FALSE;
}

/* MapArea function - create the actual map.
 * Marks room at cell (x, y) and recurses through its exits, one room step
 * (2 cells on the normal map, 1 on the world map) per exit. Cells outside
 * [min, max] are marked but not expanded. Callers must keep [min, max] at
 * least one room step inside the grid: perform_map() and str_and_map()
 * clamp the size to [1, MAX_MAP_SIZE] for that; violations are reported
 * as map/map-bounds and the cell is skipped. */
static void MapArea(room_rnum room, struct char_data *ch, int x, int y, int min, int max, sh_int xpos, sh_int ypos, bool worldmap)
{
  room_rnum prospect_room;
  struct room_direction_data *pexit;
  int door, ew_size=0, ns_size=0, x_exit_pos=0, y_exit_pos=0, step;
  sh_int prospect_xpos, prospect_ypos;

  /* Invariant: callers keep [min, max] at least MAP_MAX_STEP cells inside
   * the grid, so every cell reached (x, and x + offset for an expanded
   * cell) is in map[][]. min/max derive from the minimap size, which comes
   * from the config file and cedit; check rather than trust it. */
  if (!VALID_MAP_COORD(x, y)) {
    TB_ERROR_MSG("map", "map-bounds", "map",
                 DIAG_CHAR_FMT " cell (%d,%d) outside %dx%d grid (min %d max %d)",
                 DIAG_CHAR(ch), x, y, MAX_MAP, MAX_MAP, min, max);
    return;
  }

  if (map[x][y] < 0)
    return; /* this is a door */

  /* marks the room as visited */
  if(room == IN_ROOM(ch))
    map[x][y] = SECT_HERE;
  else
    map[x][y] = map_sector(room);

  if ( (x < min) || ( y < min) || ( x > max ) || ( y > max) ) return;

  /* Expanding probes and writes cells up to one room step away: 1 cell on
   * the world map, MAP_MAX_STEP on the normal map. (The virtual-exit branch
   * below also uses offsets[], but ns_size/ew_size are always 0 here, so it
   * never runs.) */
  step = worldmap ? 1 : MAP_MAX_STEP;
  if (!VALID_MAP_COORD(x - step, y - step) ||
      !VALID_MAP_COORD(x + step, y + step)) {
    TB_ERROR_MSG("map", "map-bounds", "map",
                 DIAG_CHAR_FMT " cell (%d,%d) too close to the %dx%d grid edge to expand (min %d max %d)",
                 DIAG_CHAR(ch), x, y, MAX_MAP, MAX_MAP, min, max);
    return;
  }

  /* Check for exits */
  for ( door = 0; door < MAX_MAP_DIR; door++ ) {

    if( door < MAX_MAP_FOLLOW &&
        xpos+door_offsets[door][0] >= 0 &&
        xpos+door_offsets[door][0] <= ns_size &&
        ypos+door_offsets[door][1] >= 0 &&
        ypos+door_offsets[door][1] <= ew_size)
    { /* Virtual exit */

      map[x+door_offsets[door][0]][y+door_offsets[door][1]] = vdoor_marks[door] ;
      if (map[x+offsets[door][0]][y+offsets[door][1]] == SECT_EMPTY )
        MapArea(room,ch,x + offsets[door][0], y + offsets[door][1], min, max, xpos+door_offsets[door][0], ypos+door_offsets[door][1], worldmap);
      continue;
    }

    if ( (pexit = world[room].dir_option[door]) != NULL  &&
         (pexit->to_room > 0 ) && map_exit_target_ok(room, door, pexit) &&
         (!IS_SET(pexit->exit_info, EX_CLOSED)) &&
         (!IS_SET(pexit->exit_info, EX_HIDDEN) || PRF_FLAGGED(ch, PRF_HOLYLIGHT)) )
    { /* A real exit */

      /* But is the door here... */
      switch (door) {
      case NORTH:
        if(xpos > 0 || ypos!=y_exit_pos) continue;
        break;
      case SOUTH:
        if(xpos < ns_size || ypos!=y_exit_pos) continue;
        break;
      case EAST:
        if(ypos < ew_size || xpos!=x_exit_pos) continue;
        break;
      case WEST:
        if(ypos > 0 || xpos!=x_exit_pos) continue;
        break;
      case NORTHWEST:
        if(xpos > 0 || ypos!=y_exit_pos || ypos > 0 || xpos!=x_exit_pos) continue;
        break;
      case NORTHEAST:
        if(xpos > 0 || ypos!=y_exit_pos || ypos < ew_size || xpos!=x_exit_pos) continue;
        break;
      case SOUTHEAST:
        if(xpos < ns_size || ypos!=y_exit_pos || ypos < ew_size || xpos!=x_exit_pos) continue;
        break;
      case SOUTHWEST:
        if(xpos < ns_size || ypos!=y_exit_pos || ypos > 0 || xpos!=x_exit_pos) continue;
        break;
      }


 /*     if ( (x < min) || ( y < min) || ( x > max ) || ( y > max) ) return;*/
      prospect_room = pexit->to_room;

        /* one way into area OR maze */
        if ( world[prospect_room].dir_option[rev_dir[door]] &&
             world[prospect_room].dir_option[rev_dir[door]]->to_room != room) {
          map[x][y] = SECT_STRANGE;
        return;
        }

      if(!worldmap) {
        if ((map[x+door_offsets[door][0]][y+door_offsets[door][1]] == DOOR_NONE) ||
            (map[x+door_offsets[door][0]][y+door_offsets[door][1]] == SECT_EMPTY)  ) {
          map[x+door_offsets[door][0]][y+door_offsets[door][1]] = door_marks[door];
        } else {
          if ( ((door == NORTHEAST) && (map[x+door_offsets[door][0]][y+door_offsets[door][1]] == DOOR_UP)) ||
               ((door == UP) && (map[x+door_offsets[door][0]][y+door_offsets[door][1]] == DOOR_DIAGNE))  ) {
            map[x+door_offsets[door][0]][y+door_offsets[door][1]] = DOOR_UP_AND_NE;
          }
          else if ( ((door == SOUTHEAST) && (map[x+door_offsets[door][0]][y+door_offsets[door][1]] == DOOR_DOWN)) ||
                    ((door == DOWN) && (map[x+door_offsets[door][0]][y+door_offsets[door][1]] == DOOR_DIAGNW))  ) {
            map[x+door_offsets[door][0]][y+door_offsets[door][1]] = DOOR_DOWN_AND_SE;
          }
        }
      }

      prospect_xpos = prospect_ypos = 0;
      switch (door) {
      case NORTH:
        prospect_xpos = ns_size;
        prospect_ypos = world[prospect_room].dir_option[rev_dir[door]] ? y_exit_pos : ew_size/2;
        break;
      case SOUTH:
        prospect_ypos = world[prospect_room].dir_option[rev_dir[door]] ? y_exit_pos : ew_size/2;
        break;
      case WEST:
        prospect_ypos = ew_size;
        prospect_xpos = world[prospect_room].dir_option[rev_dir[door]] ? x_exit_pos : ns_size/2;
        break;
      case EAST:
        prospect_xpos = world[prospect_room].dir_option[rev_dir[door]] ? x_exit_pos : ns_size/2;
        break;
      case NORTHEAST:
      case NORTHWEST:
      case SOUTHEAST:
      case SOUTHWEST:
        prospect_xpos = world[prospect_room].dir_option[rev_dir[door]] ? x_exit_pos : ns_size/2;
        prospect_ypos = world[prospect_room].dir_option[rev_dir[door]] ? y_exit_pos : ew_size/2;
        break;
      }

      if(worldmap) {
 if ( door < MAX_MAP_FOLLOW && map[x+offsets_worldmap[door][0]][y+offsets_worldmap[door][1]] == SECT_EMPTY )
   MapArea(pexit->to_room,ch,x + offsets_worldmap[door][0], y + offsets_worldmap[door][1], min, max, prospect_xpos, prospect_ypos, worldmap);
      } else {
 if ( door < MAX_MAP_FOLLOW && map[x+offsets[door][0]][y+offsets[door][1]] == SECT_EMPTY )
   MapArea(pexit->to_room,ch,x + offsets[door][0], y + offsets[door][1], min, max, prospect_xpos, prospect_ypos, worldmap);
      }
    } /* end if exit there */
  }
  return;
}

/* Returns a string representation of the map. StringMap, WorldMap and
 * CompactStringMap each return a static buffer that the next call of the
 * same function overwrites; callers copy or send it at once. */
static char *StringMap(int centre, int size)
{
  static char strmap[MAX_MAP*MAX_MAP*11 + MAX_MAP*2 + 1];
  char *mp = strmap;
  char *tmp;
  int x, y;

  /* every row */
  for (x = centre - CANVAS_HEIGHT/2; x <= centre + CANVAS_HEIGHT/2; x++) {
    /* every column */
    for (y = centre - CANVAS_WIDTH/6; y <= centre + CANVAS_WIDTH/6; y++) {
      if (abs(centre - x)<=size && abs(centre-y)<=size)
        tmp = (map[x][y]<0) ? \
       door_info[NUM_DOOR_TYPES + map[x][y]].disp : \
       map_info[map[x][y]].disp ;
      else
 tmp = map_info[SECT_EMPTY].disp;
      strlcpy(mp, tmp, sizeof(strmap) - (size_t)(mp - strmap));
      mp += strlen(mp);
    }
    strlcpy(mp, "\r\n", sizeof(strmap) - (size_t)(mp - strmap));
    mp += 2;
  }
  *mp='\0';
  return strmap;
}

static char *WorldMap(int centre, int size, int mapshape, int maptype )
{
  static char strmap[MAX_MAP*MAX_MAP*4 + MAX_MAP*2 + 1];
  char *mp = strmap;
  int x, y;
  int xmin, xmax, ymin, ymax;

  switch(maptype) {
    case MAP_COMPACT:
      xmin = centre - size;
      xmax = centre + size;
      ymin = centre - 2*size;
      ymax = centre + 2*size;
      break;
    default:
      xmin = centre - CANVAS_HEIGHT/2;
      xmax = centre + CANVAS_HEIGHT/2;
      ymin = centre - CANVAS_WIDTH/2;
      ymax = centre + CANVAS_WIDTH/2;
  }


  /* every row */
  /* for (x = centre - size; x <= centre + size; x++) { */
  for (x = xmin; x <= xmax; x++) {
    /* every column */
    /* for (y = centre - (2*size) ; y <= centre + (2*size) ; y++) {  */
    for (y = ymin ; y <= ymax ; y++) {

      if((mapshape == MAP_RECTANGLE && abs(centre - y) <= size*2  && abs(centre - x) <= size ) ||
   ((mapshape == MAP_CIRCLE) && (centre-x)*(centre-x) + (centre-y)*(centre-y)/4 <= (size * size + 1))) {
        strlcpy(mp, world_map_info[map[x][y]].disp,
                sizeof(strmap) - (size_t)(mp - strmap));
        mp += strlen(mp);
      } else {
 *mp++ = ' ';
      }
    }
    strlcpy(mp, "\tn\r\n", sizeof(strmap) - (size_t)(mp - strmap));
    mp += 4;
  }
  *mp='\0';
  return strmap;
}

static char *CompactStringMap(int centre, int size)
{
  static char strmap[MAX_MAP*MAX_MAP*12 + MAX_MAP*2 + 1];
  char *mp = strmap;
  int x, y;

  /* every row */
  for (x = centre - size; x <= centre + size; x++) {
    /* every column */
    for (y = centre - size; y <= centre + size; y++) {
      strlcpy(mp, (map[x][y] < 0) ?
               compact_door_info[NUM_DOOR_TYPES + map[x][y]].disp :
               map_info[map[x][y]].disp,
               sizeof(strmap) - (size_t)(mp - strmap));
      mp += strlen((map[x][y]<0) ? \
       compact_door_info[NUM_DOOR_TYPES + map[x][y]].disp : \
       map_info[map[x][y]].disp);
    }
    strlcpy(mp, "\r\n", sizeof(strmap) - (size_t)(mp - strmap));
    mp += 2;
  }
  *mp='\0';
  return strmap;
}

/* Display a nicely formatted map with a legend */
static void perform_map( struct char_data *ch, char *argument, bool worldmap )
{
  int size = DEFAULT_MAP_SIZE;
  int centre, x, y, min, max;
  char arg1[MAX_INPUT_LENGTH], arg2[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH], buf1[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];
  int count = 0;
  int ew_size=0, ns_size=0;
  int mapshape = MAP_CIRCLE;

  two_arguments( argument, arg1 , arg2 );
  if(*arg1)
  {
    size = tb_parse_int_or_zero(arg1);
  }
  if (*arg2)
  {
    if (is_abbrev(arg2, "normal")) worldmap=FALSE;
    else if (is_abbrev(arg2, "world")) worldmap=TRUE;
    else {
      send_to_char(ch, "Usage: \tymap <distance> [ normal | world ]\tn");
      return;
    }
  }

  if(size<0) {
    size = -size;
    mapshape = MAP_RECTANGLE;
  }
  size = URANGE(1,size,MAX_MAP_SIZE);

  centre = MAX_MAP/2;

  if(worldmap) {
    min = centre - 2*size;
    max = centre + 2*size;
  } else {
    min = centre - size;
    max = centre + size;
  }

  /* Blank the map */
  for (x = 0; x < MAX_MAP; ++x)
      for (y = 0; y < MAX_MAP; ++y)
           map[x][y]= (!(y%2) && !worldmap) ? DOOR_NONE : SECT_EMPTY;

  /* starts the mapping with the centre room */
  MapArea(IN_ROOM(ch), ch, centre, centre, min, max, ns_size/2, ew_size/2, worldmap);

  /* marks the center, where ch is */
  map[centre][centre] = SECT_HERE;

  /* Feel free to put your own MUD name or header in here */
  send_to_char(ch, " \tY-\tytbaMUD Map System\tY-\tn\r\n"
                   "\tD  .-.__--.,--.__.-.\tn\r\n" );

  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn\tn\tn%s Up\\\\", door_info[NUM_DOOR_TYPES + DOOR_UP].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn\tn\tn%s Down\\\\", door_info[NUM_DOOR_TYPES + DOOR_DOWN].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s You\\\\", map_info[SECT_HERE].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Inside\\\\", map_info[SECT_INSIDE].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s City\\\\", map_info[SECT_CITY].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Field\\\\", map_info[SECT_FIELD].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Forest\\\\", map_info[SECT_FOREST].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Hills\\\\", map_info[SECT_HILLS].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Mountain\\\\", map_info[SECT_MOUNTAIN].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Swim\\\\", map_info[SECT_WATER_SWIM].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Boat\\\\", map_info[SECT_WATER_NOSWIM].disp);
  count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Flying\\\\", map_info[SECT_FLYING].disp);
  tb_snprintf(buf + count, sizeof(buf) - (size_t)count, "\tn%s Underwater\\\\", map_info[SECT_UNDERWATER].disp);

  strlcpy(buf, strfrmt(buf, LEGEND_WIDTH, CANVAS_HEIGHT + 2, FALSE, TRUE, TRUE), sizeof(buf));

  /* Start with an empty column */
  strlcpy(buf1, strfrmt("",0, CANVAS_HEIGHT + 2, FALSE, FALSE, TRUE), sizeof(buf1));

  /* Paste the legend */
  strlcpy(buf2, strpaste(buf1, buf, "\tD | \tn"), sizeof(buf2));

  /* Set up the map */
  memset(buf, ' ', CANVAS_WIDTH);
  count = (CANVAS_WIDTH);
  if(worldmap)
    count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\r\n%s", WorldMap(centre, size, mapshape, MAP_NORMAL));
  else
    count += snprintf(buf + count, sizeof(buf) - (size_t)count, "\r\n%s", StringMap(centre, size));
  memset(buf + count, ' ', CANVAS_WIDTH);
  strlcpy(buf + count + CANVAS_WIDTH, "\r\n", sizeof(buf) - (size_t)(count + CANVAS_WIDTH));
  /* Paste it on */
  strlcpy(buf2, strpaste(buf2, buf, "\tD | \tn"), sizeof(buf2));
  /* Paste on the right border */
  strlcpy(buf2, strpaste(buf2, buf1, "  "), sizeof(buf2));
  /* Print it all out */
  send_to_char(ch, "%s", buf2);

  send_to_char(ch, "\tD `.-.__--.,-.__.-.-'\tn\r\n");
  return;
}

/* Display a string with the map beside it (the automap beside a room
 * description). target_room is an RNUM despite its room_vnum type (the only
 * caller, look_at_room(), passes IN_ROOM). The size comes from
 * CONFIG_MINIMAP_SIZE (config file / cedit, any int) and is clamped here. */
void str_and_map(char *str, struct char_data *ch, room_vnum target_room ) {
  int size, centre, x, y, min, max, char_size;
  int ew_size=0, ns_size=0;
  bool worldmap;

  /* Check MUDs map config options - if disabled, just show room decsription */
  if (!can_see_map(ch)) {
    send_to_char(ch, "%s", strfrmt(str, GET_SCREEN_WIDTH(ch), 1, FALSE, FALSE, FALSE));
    return;
  }

  worldmap = show_worldmap(ch);

  if(!PRF_FLAGGED(ch, PRF_AUTOMAP)) {
    send_to_char(ch, "%s", strfrmt(str, GET_SCREEN_WIDTH(ch), 1, FALSE, FALSE, FALSE));
    return;
  }

  /* CONFIG_MINIMAP_SIZE comes from the config file (any int) or cedit.
   * Clamp it as perform_map() does and say so: an unclamped size let
   * MapArea read and write outside map[][]. */
  size = URANGE(1, CONFIG_MINIMAP_SIZE, MAX_MAP_SIZE);
  if (size != CONFIG_MINIMAP_SIZE)
    TB_WARN_MSG("map", "minimap-size-clamp", "config",
                "minimap size %d outside [1,%d]; using %d",
                CONFIG_MINIMAP_SIZE, MAX_MAP_SIZE, size);
  centre = MAX_MAP/2;
  /* Same reach as perform_map(): rooms are 1 cell apart on the world map
   * and 2 cells apart (a door between) on the normal map. */
  if (worldmap) {
    min = centre - 2*size;
    max = centre + 2*size;
  } else {
    min = centre - size;
    max = centre + size;
  }

  for (x = 0; x < MAX_MAP; ++x)
    for (y = 0; y < MAX_MAP; ++y)
      map[x][y]= (!(y%2) && !worldmap) ? DOOR_NONE : SECT_EMPTY;

  /* starts the mapping with the center room */
MapArea(target_room, ch, centre, centre, min, max, ns_size/2, ew_size/2, worldmap ); 
  map[centre][centre] = SECT_HERE;

  /* char_size = rooms + doors + padding */
  if(worldmap)
    char_size = size * 4 + 5;
  else
    char_size = 3*(size+1) + (size) + 4;

  if(worldmap)
    send_to_char(ch, "%s", strpaste(strfrmt(str, GET_SCREEN_WIDTH(ch) - char_size, size*2 + 1, FALSE, TRUE, TRUE), WorldMap(centre, size, MAP_CIRCLE, MAP_COMPACT), " \tn"));
  else
    send_to_char(ch, "%s", strpaste(strfrmt(str, GET_SCREEN_WIDTH(ch) - char_size, size*2 + 1, FALSE, TRUE, TRUE), CompactStringMap(centre, size), " \tn"));

}

static bool show_worldmap(struct char_data *ch) {
  room_rnum rm = IN_ROOM(ch);
  zone_rnum zn = GET_ROOM_ZONE(rm);

  if (ROOM_FLAGGED(rm, ROOM_WORLDMAP)) return TRUE;
  if (ZONE_FLAGGED(zn, ZONE_WORLDMAP)) return TRUE;

  return FALSE;
}

ACMD(do_map) {
  if (!can_see_map(ch)) {
    send_to_char(ch, "Sorry, the map is disabled!\r\n");
    return;
  }
  if (IS_DARK(IN_ROOM(ch)) && !CAN_SEE_IN_DARK(ch)) {
    send_to_char(ch, "It is too dark to see the map.\r\n");
    return;
  } else if (AFF_FLAGGED(ch, AFF_BLIND) && GET_LEVEL(ch) < LVL_IMMORT) {
    send_to_char(ch, "You can't see the map while blind!\r\n");
    return;
  }
  perform_map(ch, argument, show_worldmap(ch));
}
