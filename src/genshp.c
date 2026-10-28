/**************************************************************************
*  File: genshp.c                                          Part of tbaMUD *
*  Usage: Generic OLC Library - Shops.                                    *
*                                                                         *
*  Copyright 1996 by Harvey Gilpin, 1997-2001 by George Greer.            *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "shop.h"
#include "genolc.h"
#include "genshp.h"
#include "genzon.h"

/**
 * @file genshp.c
 * Shop table maintenance for OLC (sedit) and the shop file writer.
 *
 * Input: shop data comes from builders through sedit; keeper messages are
 * free text typed by the builder.
 *
 * List conventions: in_room and producing are IDXTYPE arrays terminated by
 * NOTHING; the buy-type list is a shop_buy_data array terminated by an
 * entry whose type is NOTHING (BUY_TYPE()).  producing is declared as
 * obj_vnum * in shop.h but holds object prototype RNUMs while the server
 * runs (converted from vnums at boot, back to vnums by save_shops()), so
 * delete_object()/add_object() renumber it.  keeper is a mob RNUM (or
 * NOBODY).  Stored rnums are range-checked before save_shops() indexes
 * with them.
 *
 * Ownership: each shop_index[] entry owns its lists and message strings;
 * see add_shop() for the one place where a slot is filled without freeing.
 */

/* NOTE (gg): Didn't modify sedit much. Don't consider it as 'recent' as the
 * other editors with regard to updates or style. */

/* local (file scope) functions */
static void copy_shop_list(IDXTYPE **tlist, IDXTYPE *flist);
static void copy_shop_type_list(struct shop_buy_data **tlist, struct shop_buy_data *flist);
static void free_shop_strings(struct shop_data *shop);
static void free_shop_type_list(struct shop_buy_data **list);

void copy_shop(struct shop_data *tshop, struct shop_data *fshop, int free_old_strings)
{
  /* Copy basic information over. */
  S_NUM(tshop) = S_NUM(fshop);
  S_KEEPER(tshop) = S_KEEPER(fshop);
  S_OPEN1(tshop) = S_OPEN1(fshop);
  S_CLOSE1(tshop) = S_CLOSE1(fshop);
  S_OPEN2(tshop) = S_OPEN2(fshop);
  S_CLOSE2(tshop) = S_CLOSE2(fshop);
  S_BANK(tshop) = S_BANK(fshop);
  S_BROKE_TEMPER(tshop) = S_BROKE_TEMPER(fshop);
  S_BITVECTOR(tshop) = S_BITVECTOR(fshop);
  S_NOTRADE(tshop) = S_NOTRADE(fshop);
  S_SORT(tshop) = S_SORT(fshop);
  S_BUYPROFIT(tshop) = S_BUYPROFIT(fshop);
  S_SELLPROFIT(tshop) = S_SELLPROFIT(fshop);
  S_FUNC(tshop) = S_FUNC(fshop);

  /* Copy lists over. */
  copy_shop_list(&(S_ROOMS(tshop)), S_ROOMS(fshop));
  copy_shop_list(&(S_PRODUCTS(tshop)), S_PRODUCTS(fshop));
  copy_shop_type_list(&(tshop->type), fshop->type);

  /* Copy notification strings over. */
  if (free_old_strings)
    free_shop_strings(tshop);
  S_NOITEM1(tshop) = str_udup(S_NOITEM1(fshop));
  S_NOITEM2(tshop) = str_udup(S_NOITEM2(fshop));
  S_NOCASH1(tshop) = str_udup(S_NOCASH1(fshop));
  S_NOCASH2(tshop) = str_udup(S_NOCASH2(fshop));
  S_NOBUY(tshop) = str_udup(S_NOBUY(fshop));
  S_BUY(tshop) = str_udup(S_BUY(fshop));
  S_SELL(tshop) = str_udup(S_SELL(fshop));

}

/* Copy a 'NOTHING' terminated integer array list. */
static void copy_shop_list(IDXTYPE **tlist, IDXTYPE *flist)
{
  int num_items, i;

  if (*tlist)
    free(*tlist);

  /* Count number of entries. */
  for (i = 0; flist[i] != NOTHING; i++);
  num_items = i + 1;

  /* Make space for entries. */
  CREATE(*tlist, IDXTYPE, num_items);

  /* Copy entries over. */
  for (i = 0; i < num_items; i++)
    (*tlist)[i] = flist[i];
}

/* Copy a -1 terminated (in the type field) shop_buy_data array list. */
static void copy_shop_type_list(struct shop_buy_data **tlist, struct shop_buy_data *flist)
{
  int num_items, i;

  if (*tlist)
    free_shop_type_list(tlist);

  /* Count number of entries. */
  for (i = 0; BUY_TYPE(flist[i]) != NOTHING; i++);
  num_items = i + 1;

  /* Make space for entries. */
  CREATE(*tlist, struct shop_buy_data, num_items);

  /* Copy entries over. */
  for (i = 0; i < num_items; i++) {
    (*tlist)[i].type = flist[i].type;
    if (BUY_WORD(flist[i]))
      BUY_WORD((*tlist)[i]) = strdup(BUY_WORD(flist[i]));
  }
}

/* Remove entry num from a buy-type list.  num is builder input (sedit's
 * delete-entry prompt): one outside the list changes nothing and is
 * reported.  The caller knows which shop the list belongs to. */
void remove_shop_from_type_list(struct shop_buy_data **list, int num)
{
  int i, num_items;
  struct shop_buy_data *nlist;

  /* Count number of entries. */
  for (i = 0; (*list)[i].type != NOTHING; i++);

  if (num < 0 || num >= i) {
    TB_WARN_MSG("olc", "shop-list-index", "shop",
                "buy-type list: remove index %d out of range 0..%d; nothing removed",
                num, i - 1);
    return;
  }
  num_items = i;

  CREATE(nlist, struct shop_buy_data, num_items);

  for (i = 0; i < num_items; i++)
    nlist[i] = (i < num) ? (*list)[i] : (*list)[i + 1];

  free(BUY_WORD((*list)[num]));
  free(*list);
  *list = nlist;
}

void add_shop_to_type_list(struct shop_buy_data **list, struct shop_buy_data *newl)
{
  int i, num_items;
  struct shop_buy_data *nlist;

  /* Count number of entries. */
  for (i = 0; (*list)[i].type != NOTHING; i++);
  num_items = i;

  /* Make a new list and slot in the new entry. */
  CREATE(nlist, struct shop_buy_data, num_items + 2);

  for (i = 0; i < num_items; i++)
    nlist[i] = (*list)[i];
  nlist[num_items] = *newl;
  nlist[num_items + 1].type = NOTHING;

  /* Out with the old, in with the new. */
  free(*list);
  *list = nlist;
}

void add_shop_to_int_list(IDXTYPE **list, IDXTYPE newi)
{
  IDXTYPE i, num_items, *nlist;

  /* Count number of entries. */
  for (i = 0; (*list)[i] != NOTHING; i++);
  num_items = i;

  /* Make a new list and slot in the new entry. */
  CREATE(nlist, IDXTYPE, num_items + 2);

  for (i = 0; i < num_items; i++)
    nlist[i] = (*list)[i];
  nlist[num_items] = newi;
  nlist[num_items + 1] = NOTHING;

  /* Out with the old, in with the new. */
  free(*list);
  *list = nlist;
}

/* Remove entry num from a NOTHING-terminated room or product list.  num is
 * builder input from sedit's delete-entry prompts (a negative number
 * arrives as a large unsigned value): one outside the list changes nothing
 * and is reported. */
void remove_shop_from_int_list(IDXTYPE **list, IDXTYPE num)
{
  IDXTYPE i, num_items, *nlist;

  /* Count number of entries. */
  for (i = 0; (*list)[i] != NOTHING; i++);

#if CIRCLE_UNSIGNED_INDEX
  if (num >= i)
#else
  if (num < 0 || num >= i)
#endif
  {
    TB_WARN_MSG("olc", "shop-list-index", "shop",
                "room/product list: remove index %ld out of range 0..%ld; nothing removed",
                (long)num, (long)i - 1);
    return;
  }
  num_items = i;

  CREATE(nlist, IDXTYPE, num_items);

  for (i = 0; i < num_items; i++)
    nlist[i] = (i < num) ? (*list)[i] : (*list)[i + 1];

  free(*list);
  *list = nlist;
}

/* Free all the notice character strings in a shop structure. */
static void free_shop_strings(struct shop_data *shop)
{
  if (S_NOITEM1(shop)) {
    free(S_NOITEM1(shop));
    S_NOITEM1(shop) = NULL;
  }
  if (S_NOITEM2(shop)) {
    free(S_NOITEM2(shop));
    S_NOITEM2(shop) = NULL;
  }
  if (S_NOCASH1(shop)) {
    free(S_NOCASH1(shop));
    S_NOCASH1(shop) = NULL;
  }
  if (S_NOCASH2(shop)) {
    free(S_NOCASH2(shop));
    S_NOCASH2(shop) = NULL;
  }
  if (S_NOBUY(shop)) {
    free(S_NOBUY(shop));
    S_NOBUY(shop) = NULL;
  }
  if (S_BUY(shop)) {
    free(S_BUY(shop));
    S_BUY(shop) = NULL;
  }
  if (S_SELL(shop)) {
    free(S_SELL(shop));
    S_SELL(shop) = NULL;
  }
}

/* Free a type list and all the strings it contains. */
static void free_shop_type_list(struct shop_buy_data **list)
{
  int i;

  for (i = 0; (*list)[i].type != NOTHING; i++)
    if (BUY_WORD((*list)[i]))
      free(BUY_WORD((*list)[i]));

  free(*list);
  *list = NULL;
}

/* Free up the whole shop structure and it's content. */
void free_shop(struct shop_data *shop)
{
  free_shop_strings(shop);
  free_shop_type_list(&(S_NAMELISTS(shop)));
  free(S_ROOMS(shop));
  free(S_PRODUCTS(shop));
  free(shop);
}

/* Returns the real number of the shop with given virtual number. We take so
 * good care to keep it sorted - let's use it. - Welcor */
shop_rnum real_shop(shop_vnum vnum)
{
  int bot, top, mid;

  bot = 0;
  top = top_shop;

  /* perform binary search on shop_table */
  while (bot <= top) {
    mid = (bot + top) / 2;
    if (SHOP_NUM(mid) == vnum)
      return (mid);
    if (SHOP_NUM(mid) > vnum)
      top = mid - 1;
    else
      bot = mid + 1;
  }
  return NOWHERE;
}

/**
 * Replace a shop keeper message with builder text, prefixing "%s " when
 * the text does not start with '%' (the customer's name comes first).
 *
 * The result is a template, not a printf format: shop.c renders it with
 * shop_format_message(), which substitutes only %s (customer name), %d
 * (amount, buy/sell messages) and %%, and copies and reports anything
 * else.  So no format validation is needed here, and a message must never
 * be passed to a printf-family function as the format.
 *
 * @param str   Message field to replace; the old string is freed.
 * @param new_s Builder text from sedit.
 */
void modify_shop_string(char **str, char *new_s)
{

  char buf[MAX_STRING_LENGTH];
  char *pointer;

  /* Check the '%s' is present, if not, add it. */
  if (*new_s != '%') {
    tb_snprintf(buf, sizeof(buf), "%%s %s", new_s);
    pointer = buf;
  } else
    pointer = new_s;

  if (*str)
    free(*str);
  *str = strdup(pointer);
}

/**
 * Add a new shop or update the one with the same vnum, from sedit's copy.
 *
 * Update: the live entry's lists and strings are freed and replaced by deep
 * copies.  Insert: shop_index[] grows by one and later shops shift up by
 * struct copy, so each shifted slot's pointers now belong to the slot
 * above.  The new slot is therefore filled with copy_shop(..., FALSE)
 * after its list pointers are cleared: its old strings are still owned by
 * the neighbour and must not be freed.  nshp keeps its own copies.
 *
 * @return The shop's rnum.  A vnum outside every zone is reported to
 *         builders and the shop is not queued for saving.
 */
int add_shop(struct shop_data *nshp)
{
  shop_rnum rshop;
  int found = 0;
  zone_rnum rznum = real_zone_by_thing(S_NUM(nshp));

  /* The shop already exists, just update it. */
  if ((rshop = real_shop(S_NUM(nshp))) != NOWHERE) {
   /* free old strings. They're not used in any other place -- Welcor */
   copy_shop(&shop_index[rshop], nshp, TRUE);
    if (rznum != NOWHERE)
      add_to_save_list(zone_table[rznum].number, SL_SHP);
    else
      mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: GenOLC: Cannot determine shop zone.");
    return rshop;
  }

  top_shop++;
  RECREATE(shop_index, struct shop_data, top_shop + 1);

  for (rshop = top_shop; rshop > 0; rshop--) {
    if (nshp->vnum > SHOP_NUM(rshop - 1)) {
      found = rshop;

      /* Make a "nofree" variant and remove these later. */
      shop_index[rshop].in_room = NULL;
      shop_index[rshop].producing = NULL;
      shop_index[rshop].type = NULL;
      /* don't free old strings - they're still in use -- Welcor */
      copy_shop(&shop_index[rshop], nshp, FALSE);
      break;
    }
    shop_index[rshop] = shop_index[rshop - 1];
  }

  if (!found) {
    /* Make a "nofree" variant and remove these later. */
    shop_index[rshop].in_room = NULL;
    shop_index[rshop].producing = NULL;
    shop_index[rshop].type = NULL;
    /* don't free old strings - they're still in use -- Welcor */
    copy_shop(&shop_index[0], nshp, FALSE);
  }

  if (rznum != NOWHERE)
    add_to_save_list(zone_table[rznum].number, SL_SHP);
  else
    mudlog(BRF, LVL_BUILDER, TRUE, "SYSERR: GenOLC: Cannot determine shop zone.");

  return rshop;
}

int save_shops(zone_rnum zone_num)
{
  int i, j, rshop, num_shops = 0;
  FILE *shop_file;
  char fname[128], oldname[128];
  struct shop_data *shop;

  if (!VALID_ZONE_RNUM(zone_num)) {
    log("SYSERR: GenOLC: save_shops: Invalid real zone number %d. (0-%d)", zone_num, top_of_zone_table);
    return FALSE;
  }

  tb_snprintf(fname, sizeof(fname), "%s/%d.new", SHP_PREFIX, zone_table[zone_num].number);
  if (!(shop_file = fopen(fname, "w"))) {
    TB_ERROR_ERRNO("olc", "shop-save-open", fname, "zone %d shops not saved",
                   zone_table[zone_num].number);
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: OLC: Cannot open shop file %s!", fname);
    return FALSE;
  } else if (fprintf(shop_file, "CircleMUD v3.0 Shop File~\n") < 0) {
    TB_ERROR_ERRNO("olc", "shop-save-write", fname, "zone %d shops not saved",
                   zone_table[zone_num].number);
    mudlog(BRF, LVL_GOD, TRUE, "SYSERR: OLC: Cannot write to shop file %s!", fname);
    tb_fclose(shop_file);
    /* never leave a header-less .new behind */
    if (remove(fname) != 0 && errno != ENOENT)
      TB_REPORT_ERRNO("remove", fname);
    return FALSE;
  }
  /* Search database for shops in this zone. */
  for (i = genolc_zone_bottom(zone_num); i <= zone_table[zone_num].top; i++) {
    if ((rshop = real_shop(i)) != NOWHERE) {
      tb_fprintf(shop_file, "#%d~\n", i);
      shop = shop_index + rshop;

      /* Save the products.  producing[] holds prototype rnums kept in step
       * by the OLC renumberers; a stale one would read past obj_index[], so
       * the whole zone save is abandoned rather than write a wrong vnum. */
      for (j = 0; S_PRODUCT(shop, j) != NOTHING; j++) {
        if (S_PRODUCT(shop, j) > top_of_objt) {
          TB_ERROR_MSG("olc", "shop-save-product", fname,
                       "shop vnum %d product %d rnum %d > top %d; zone %d not saved",
                       S_NUM(shop), j, (int)S_PRODUCT(shop, j), (int)top_of_objt,
                       zone_table[zone_num].number);
          tb_fclose(shop_file);
          if (remove(fname) != 0 && errno != ENOENT)
            TB_REPORT_ERRNO("remove", fname);
          return FALSE;
        }
	tb_fprintf(shop_file, "%d\n", obj_index[S_PRODUCT(shop, j)].vnum);
      }
      tb_fprintf(shop_file, "-1\n");

      /* Save the rates. */
      tb_fprintf(shop_file, "%1.2f\n"
                         "%1.2f\n",
                         S_BUYPROFIT(shop),
                         S_SELLPROFIT(shop));

      /* Save the buy types and namelists. */
      for (j = 0;S_BUYTYPE(shop, j) != NOTHING; j++)
        tb_fprintf(shop_file, "%d%s\n",
                S_BUYTYPE(shop, j),
		S_BUYWORD(shop, j) ? S_BUYWORD(shop, j) : "");
      tb_fprintf(shop_file, "-1\n");

      /* Save messages one at a time.  The old code assembled all seven
       * strings into one MAX_STRING_LENGTH stack buffer with sprintf(),
       * even though the file format has no such combined bound. */
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_NOITEM1(shop) ? S_NOITEM1(shop) : "%s Ke?!"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_NOITEM2(shop) ? S_NOITEM2(shop) : "%s Ke?!"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_NOBUY(shop) ? S_NOBUY(shop) : "%s Ke?!"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_NOCASH1(shop) ? S_NOCASH1(shop) : "%s Ke?!"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_NOCASH2(shop) ? S_NOCASH2(shop) : "%s Ke?!"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_BUY(shop) ? S_BUY(shop) : "%s Ke?! %d?"));
      tb_fprintf(shop_file, "%s~\n",
              convert_from_tabs(S_SELL(shop) ? S_SELL(shop) : "%s Ke?! %d?"));
      /* keeper is a mob rnum kept in step by delete_mobile(); a stale value
       * (e.g. NOBODY decremented to 65534) would read past mob_index[]. */
      if (S_KEEPER(shop) != NOBODY && S_KEEPER(shop) > top_of_mobt) {
        TB_ERROR_MSG("olc", "shop-save-keeper", fname,
                     "shop vnum %d keeper rnum %d > top %d; zone %d not saved",
                     S_NUM(shop), (int)S_KEEPER(shop), (int)top_of_mobt,
                     zone_table[zone_num].number);
        tb_fclose(shop_file);
        if (remove(fname) != 0 && errno != ENOENT)
          TB_REPORT_ERRNO("remove", fname);
        return FALSE;
      }
      tb_fprintf(shop_file, "%d\n%ld\n%d\n%d\n",
              S_BROKE_TEMPER(shop),
              S_BITVECTOR(shop),
              S_KEEPER(shop) == NOBODY ? -1 : mob_index[S_KEEPER(shop)].vnum,
              S_NOTRADE(shop));

      /* Save the rooms. */
      for (j = 0;S_ROOM(shop, j) != NOWHERE; j++)
        tb_fprintf(shop_file, "%d\n", S_ROOM(shop, j));
      tb_fprintf(shop_file, "-1\n");

      /* Save open/closing times. */
      tb_fprintf(shop_file, "%d\n%d\n%d\n%d\n", S_OPEN1(shop), S_CLOSE1(shop),
          S_OPEN2(shop), S_CLOSE2(shop));
      num_shops++;
    }
  }
  tb_fprintf(shop_file, "$~\n");
  {
    /* ferror() stays set after any failed write.  tb_fprintf() reported
     * each one, but a later flush that succeeds would hide it and commit a
     * file with a hole in it (C2-16). */
    int stream_ok = !ferror(shop_file);
    int flush_ok = tb_fflush_checked(shop_file, fname);
    int close_ok = tb_fclose_checked(shop_file, fname);
    if (!stream_ok)
      TB_ERROR_MSG("olc", "save-stream-error", fname,
                   TB_ERRID_PERSIST_WRITE " zone %d shops: a write failed; previous file kept",
                   zone_table[zone_num].number);
    if (!stream_ok || !flush_ok || !close_ok) {
      if (remove(fname) != 0 && errno != ENOENT)
        TB_REPORT_ERRNO("remove", fname);
      return FALSE;
    }
  }
  tb_snprintf(oldname, sizeof(oldname), "%s/%d.shp", SHP_PREFIX, zone_table[zone_num].number);
  if (!tb_replace_file_checked(fname, oldname))
    return FALSE;

  /* The .shp file is committed, but without an index entry it is not
   * loaded at boot: keep the zone in the save list so a later save retries. */
  if (num_shops > 0 && !create_world_index(zone_table[zone_num].number, "shp"))
    return FALSE;

  if (in_save_list(zone_table[zone_num].number, SL_SHP))
    remove_from_save_list(zone_table[zone_num].number, SL_SHP);
  return TRUE;
}
