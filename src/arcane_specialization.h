#ifndef TBAMUD_ARCANE_SPECIALIZATION_H
#define TBAMUD_ARCANE_SPECIALIZATION_H

/* Pure casting calculations shared by live combat and native regression tests.
 * Inputs are already validated by the spell system. */
static inline int arcane_adjust_mana(int class_id, int level,
                                    int minimum_level, int base_mana,
                                    int damaging_spell, int is_npc)
{
  if (is_npc || base_mana <= 0)
    return base_mana;

  if (class_id == CLASS_MAGIC_USER && level >= 20 &&
      level - minimum_level >= 10)
    return base_mana <= 1 ? 1 : (base_mana * 4 + 4) / 5;

  if (class_id == CLASS_SORCERER && level >= 20 && damaging_spell)
    return (base_mana * 3 + 1) / 2;

  return base_mana;
}

static inline int arcane_adjust_damage(int class_id, int level, int damage,
                                      int casttype, int is_npc)
{
  if (!is_npc && class_id == CLASS_SORCERER &&
      level >= 20 && casttype == CAST_SPELL && damage > 0)
    return damage + damage / 4;
  return damage;
}
#endif
