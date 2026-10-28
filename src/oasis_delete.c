/**************************************************************************
*  File: oasis_delete.c                                    Part of tbaMUD *
*  Usage: Oasis OLC deletion.                                             *
*                                                                         *
* By Levork. Copyright 1996 Harvey Gilpin. 1997-2001 George Greer.        *
* 2002 Kip Potter [Mythran].                                              *
**************************************************************************/

/**
 * @file oasis_delete.c
 * free_strings(): release the heap strings of an OLC scratch room or
 * configuration.  The server's only caller is free_config() in oasis.c
 * (cedit cleanup through cleanup_olc); rooms are released with redit's
 * free_room(), which uses free_room_strings() instead.  The room cases are
 * kept for API compatibility and for tests.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "genolc.h"
#include "oasis.h"
#include "improved-edit.h"

/** Free the strings of a scratch room or configuration.
 *
 * @param data A struct room_data (OASIS_WLD, OASIS_EXI) or struct
 *             config_data (OASIS_CFG), as selected by type.
 * @param type OASIS_WLD frees name, description, extra descriptions and
 *             then the exits (as OASIS_EXI).  OASIS_EXI frees every exit and
 *             its strings and NULLs the dir_option slots.  OASIS_CFG frees
 *             the play.* messages and the operation.* strings except
 *             CONFFILE.  OASIS_MOB and OASIS_OBJ are not implemented and
 *             return FALSE without freeing anything.
 * @return TRUE if the strings were freed, FALSE otherwise.
 *
 * Only the exits are reset to NULL.  For OASIS_WLD and OASIS_CFG the freed
 * string fields are left dangling, so the caller must discard (free) the
 * struct immediately, as free_room and free_config do; reusing it would
 * free those strings twice. */
int free_strings(void *data, int type)
{
  struct room_data *room;
  struct config_data *config;
  int i;

  switch (type) {
    case OASIS_WLD:
      room = (struct room_data *) data;

      /* Free Descriptions */
      if (room->name)
        free(room->name);

      if (room->description)
        free(room->description);

      if (room->ex_description)
        free_ex_descriptions(room->ex_description);

      /* Return the return value of free_strings(). */
      return (free_strings(room, OASIS_EXI));

    case OASIS_EXI:
      room = (struct room_data *) data;

      for (i = 0; i < NUM_OF_DIRS; i++) { /* NUM_OF_DIRS, not DIR_COUNT */
        if (room->dir_option[i]) {
          if (room->dir_option[i]->general_description) {
            free(room->dir_option[i]->general_description);
            room->dir_option[i]->general_description = NULL;
          }
          if (room->dir_option[i]->keyword) {
            free(room->dir_option[i]->keyword);
            room->dir_option[i]->keyword = NULL;
          }
          free(room->dir_option[i]);
          room->dir_option[i] = NULL;
        }
      }

      return (TRUE);

    case OASIS_MOB:
    case OASIS_OBJ:
      return (FALSE);		/* For now... */

    case OASIS_CFG:
      config = (struct config_data *) data;

      if (config->play.OK)
        free(config->play.OK);

      if (config->play.HUH)
        free(config->play.HUH);

      if (config->play.NOPERSON)
        free(config->play.NOPERSON);

      if (config->play.NOEFFECT)
        free(config->play.NOEFFECT);

      if (config->operation.DFLT_IP)
        free(config->operation.DFLT_IP);

      if (config->operation.DFLT_DIR)
        free(config->operation.DFLT_DIR);

      if (config->operation.LOGNAME)
        free(config->operation.LOGNAME);

      if (config->operation.MENU)
        free(config->operation.MENU);

      if (config->operation.WELC_MESSG)
        free(config->operation.WELC_MESSG);

      if (config->operation.START_MESSG)
        free(config->operation.START_MESSG);

      return (TRUE);

    default:
      mudlog(BRF, LVL_GOD, TRUE, "SYSERR: oasis_delete.c: free_strings: Invalid type handled (Type %d).", type);
      return (FALSE);
  }
}
