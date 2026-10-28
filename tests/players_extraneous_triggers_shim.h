/* Force-included (-include) ahead of src/players.c by the CTest
 * test_players_extraneous_triggers_rejected (B2-21). It includes
 * dg_scripts.h first and then undefines NO_EXTRANEOUS_TRIGGERS, as a
 * builder who wants save_char() to fire wear/remove triggers would; the
 * include guard stops players.c's own #include from defining it again.
 * players.c must then stop at its B2-21 #error rather than compile a
 * save_char() that runs scripts while it holds the worn items.
 */
#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "handler.h"
#include "pfdefaults.h"
#include "dg_scripts.h"
#undef NO_EXTRANEOUS_TRIGGERS
