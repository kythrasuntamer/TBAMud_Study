/**
* @file olc_checks.h
* Invariant checks shared by the OasisOLC prototype editors (oedit.c, medit.c,
* redit.c, zedit.c, sedit.c).
*
* Every OLC session edits a private scratch copy (OLC_OBJ, OLC_MOB, OLC_ROOM,
* OLC_ZONE, OLC_SHOP) that refers to other prototypes by *real* number: a
* position in obj_proto[], mob_proto[] or world[]. Those positions shift
* whenever any builder inserts or deletes a prototype, so an rnum held by an
* open session can go stale while its builder is idle.
*
* - olc_renumber_sessions() is the one place that moves every open scratch
*   copy in step with an insert or delete. Call it after each add_* that
*   inserted and after each successful delete_*.
* - OLC_CHECK_RNUM() guards every place that indexes a table with a scratch
*   rnum; a value that missed a renumber is reported (op "stale-rnum")
*   instead of being read out of bounds.
*
* Input ownership: the rnums come from builders (OLC menus) and from other
* builders' concurrent edits; none of them may be trusted as an index.
*/
#ifndef TBAMUD_OLC_CHECKS_H_INCLUDED
#define TBAMUD_OLC_CHECKS_H_INCLUDED

/* Range checks for rnums held as plain numbers (zone command args, shop
 * lists, scratch identities). VALID_ROOM_RNUM is in utils.h; VALID_OBJ_RNUM
 * there takes an object pointer, hence the _NUM suffix here. The casts make
 * the checks correct for int, signed and unsigned index types alike, and
 * NOWHERE/NOTHING/NOBODY always fail them. Each evaluates r twice: pass a
 * plain value. */
#define VALID_MOB_RNUM(r)     ((long)(r) >= 0 && (long)(r) <= (long)top_of_mobt)
#define VALID_OBJ_RNUM_NUM(r) ((long)(r) >= 0 && (long)(r) <= (long)top_of_objt)
/* Token-identical to dg_scripts.h's definition (rejects NOTHING and empty
 * index slots), so including both headers is not a redefinition. */
#define VALID_TRIG_RNUM(rn) \
  ((rn) != NOTHING && (int)(rn) < top_of_trigt && trig_index[(rn)] != NULL)

/* The rnum tables an OLC scratch copy can refer to. */
#define OLC_RNUM_OBJ   0
#define OLC_RNUM_MOB   1
#define OLC_RNUM_ROOM  2
#define OLC_RNUM_TRIG  3

/* Renumber one stored rnum after a table insert or delete at refpt. 'none'
 * is the sentinel (NOWHERE/NOTHING/NOBODY) that must never move. A delete
 * of var itself is the caller's job (test var == refpt first). var is
 * evaluated three times: pass a plain lvalue. */
#define RENUM_ON_INSERT(var, refpt, none) \
  ((var) += ((var) != (none) && (var) >= (refpt)))
#define RENUM_ON_DELETE(var, refpt, none) \
  ((var) -= ((var) != (none) && (var) > (refpt)))

/* Walk the descriptors that have an OLC session in the given CON_ state.
 * The trailing-else form keeps a following 'else' from binding to the
 * hidden 'if'. state is evaluated once per descriptor. */
#define FOR_EACH_OLC_SESSION(dsc, state) \
  for ((dsc) = descriptor_list; (dsc); (dsc) = (dsc)->next) \
    if (STATE(dsc) != (state) || !(dsc)->olc) ; else

/** Store builder input clamped to [lo, hi] in lval. A value outside the
 * range is never changed silently: the builder is told what was stored and
 * an "olc-clamp" warning names the call site, builder, edited vnum, field,
 * input and stored value. Replaces LIMIT() for builder input, which clamps
 * silently and evaluates its first argument up to three times. input, lo
 * and hi are evaluated once; d several times; field is a string literal. */
#define OLC_SET_CLAMPED(d, lval, input, lo, hi, field) do { \
    long olc_in_ = (long)(input), olc_lo_ = (long)(lo), olc_hi_ = (long)(hi); \
    long olc_v_ = olc_in_ < olc_lo_ ? olc_lo_ : (olc_in_ > olc_hi_ ? olc_hi_ : olc_in_); \
    if (olc_v_ != olc_in_) { \
      struct char_data *olc_builder_ = (d)->character; \
      write_to_output((d), "%s %ld is out of range [%ld..%ld]; set to %ld.\r\n", \
                      (field), olc_in_, olc_lo_, olc_hi_, olc_v_); \
      TB_WARN_MSG("olc", "olc-clamp", (field), \
                  "builder " DIAG_CHAR_FMT " vnum %d %s input %ld clamped to %ld", \
                  DIAG_CHAR(olc_builder_), OLC_NUM(d), (field), olc_in_, olc_v_); \
    } \
    (lval) = olc_v_; \
  } while (0)

/** A builder starting an OLC session must not already have one: every exit
 * path ends in cleanup_olc(), which frees d->olc. If one is found anyway,
 * report olc-stale-state with the builder and the stale session's mode and
 * vnum, and release it fully (a bare free(d->olc) leaked its scratch copy
 * and trigger list). Statement macro; ch is the builder. */
#define OLC_DISCARD_STALE_SESSION(d, ch) do { \
    TB_ERROR_MSG("olc", "olc-stale-state", "descriptor", \
                 "builder " DIAG_CHAR_FMT " stale olc mode %d vnum %d; discarded", \
                 DIAG_CHAR(ch), OLC_MODE(d), OLC_NUM(d)); \
    cleanup_olc((d), CLEANUP_ALL); \
  } while (0)

/** Report a scratch rnum outside its table as a "stale-rnum" error naming the
 * call site, builder, edited vnum and table. Returns TRUE when r is valid. */
#define OLC_CHECK_RNUM(d, kind, r) \
  olc_check_rnum_at(__FILE__, __LINE__, __func__, (d), (kind), (long)(r))

int olc_check_rnum_at(const char *file, int line, const char *func,
                      struct descriptor_data *d, int kind, long rnum);

/** Report an index into a name table (genders, position_types, dirs,
 * equipment_types, item_types, attack_hit_text...) that is outside
 * [0, count) as a "table-index" error naming the call site, builder,
 * edited vnum, table and value. Returns TRUE when idx is valid. The values
 * come from world files, which the loaders do not range-check. */
#define OLC_CHECK_INDEX(d, table, idx, count) \
  olc_check_index_at(__FILE__, __LINE__, __func__, (d), (table), (long)(idx), (long)(count))

/** table[idx] for a menu, or "<invalid>" after OLC_CHECK_INDEX reported
 * it. idx is evaluated twice: pass a plain value. Elson-style accessor in
 * the spirit of sprinttype(), but bounded by the table's real size. */
#define OLC_TABLE_NAME(d, table, count, idx) \
  (OLC_CHECK_INDEX((d), #table, (idx), (count)) ? (table)[(idx)] : "<invalid>")

int olc_check_index_at(const char *file, int line, const char *func,
                       struct descriptor_data *d, const char *table,
                       long idx, long count);
void olc_renumber_sessions(int kind, int refpt, int delta,
                           struct descriptor_data *skip);
/** Free OLC_SCRIPT(d) unless a save already handed it to a prototype (save
 * paths set it to NULL). Called by cleanup_olc() and before 'W' re-setup. */
void olc_free_script_copy(struct descriptor_data *d);

#endif /* TBAMUD_OLC_CHECKS_H_INCLUDED */
