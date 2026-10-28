#ifndef TBAMUD_ARCANE_SPECIALIZATION_H
#define TBAMUD_ARCANE_SPECIALIZATION_H

/* Uses CLASS_* (structs.h); conf.h and sysdep.h come first in every
 * translation unit, as everywhere in the codebase. */
#include "structs.h"

/* Pure casting calculations shared by live combat and native regression tests.
 * Inputs are already validated by the spell system.  minimum_mana is the
 * spell's mana_min: a discount never takes the cost below it. */
static inline int arcane_adjust_mana(int class_id, int level,
                                    int minimum_level, int base_mana,
                                    int damaging_spell, int is_npc,
                                    int minimum_mana)
{
  if (is_npc || base_mana <= 0)
    return base_mana;

  if (class_id == CLASS_MAGIC_USER && level >= 20 &&
      level - minimum_level >= 10) {
    int discounted = base_mana <= 1 ? 1 : (base_mana * 4 + 4) / 5;
    return discounted < minimum_mana ? minimum_mana : discounted;
  }

  if (class_id == CLASS_SORCERER && level >= 20 && damaging_spell)
    return (base_mana * 3 + 1) / 2;

  return base_mana;
}

/* direct_cast: the spell was cast (CAST_SPELL), not used from a wand,
 * staff, scroll or potion, nor an area effect. */
static inline int arcane_adjust_damage(int class_id, int level, int damage,
                                      int direct_cast, int is_npc)
{
  if (!is_npc && class_id == CLASS_SORCERER &&
      level >= 20 && direct_cast && damage > 0)
    return damage + damage / 4;
  return damage;
}
#endif
