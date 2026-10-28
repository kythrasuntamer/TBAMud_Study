/**
* @file lists.h
* Lists Header file.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
* 
* This source code, which was not part of the CircleMUD legacy code,
* is attributed to:
* Copyright 2012 by Joseph Arnusch.                                                    
*/

#ifndef TBAMUD_LISTS_H_INCLUDED
#define TBAMUD_LISTS_H_INCLUDED

struct item_data {
  struct item_data * pPrevItem;
  struct item_data * pNextItem;
  void             * pContent;
};

struct list_data {
  struct item_data * pFirstItem;
  struct item_data * pLastItem;
  unsigned short int iIterators;
  unsigned short int iSize;
};

struct iterator_data {
  struct list_data * pList;
  struct item_data * pItem;
};

/* Externals */
extern struct list_data * global_lists;
extern struct list_data * group_list;

/* Locals */
void * random_from_list(struct list_data * pList);
struct list_data * randomize_list(struct list_data * pList);
struct list_data * create_list(void);
struct item_data * find_in_list(void * pContent, struct list_data * pList);
void clear_simple_list(void);

/* The functions that can detect a broken list invariant take the caller's
 * location, so their diagnostics name the code that misused the list rather
 * than lists.c. Call them through the macros below, exactly as before. */
void add_to_list_at(void * pContent, struct list_data * pList,
                    const char *file, int line, const char *func);
void * merge_iterator_at(struct iterator_data * pIterator, struct list_data * pList,
                         const char *file, int line, const char *func);
void remove_iterator_at(struct iterator_data * pIterator,
                        const char *file, int line, const char *func);
void * next_in_list_at(struct iterator_data * pIterator,
                       const char *file, int line, const char *func);
void remove_from_list_at(void * pContent, struct list_data * pList,
                         const char *file, int line, const char *func);
void * simple_list_at(struct list_data * pList,
                      const char *file, int line, const char *func);
void free_list_at(struct list_data * pList,
                  const char *file, int line, const char *func);

#define add_to_list(c, l)       add_to_list_at((c), (l), __FILE__, __LINE__, __func__)
#define merge_iterator(it, l)   merge_iterator_at((it), (l), __FILE__, __LINE__, __func__)
#define remove_iterator(it)     remove_iterator_at((it), __FILE__, __LINE__, __func__)
#define next_in_list(it)        next_in_list_at((it), __FILE__, __LINE__, __func__)
#define remove_from_list(c, l)  remove_from_list_at((c), (l), __FILE__, __LINE__, __func__)
#define simple_list(l)          simple_list_at((l), __FILE__, __LINE__, __func__)
#define free_list(l)            free_list_at((l), __FILE__, __LINE__, __func__)

/** Loop over list taking its FIRST content each pass, for loops whose body
 * removes that content (cancel/free loops). No iterator or global state is
 * involved, and list is re-read every pass, so the body may also free the
 * list provided it sets the list variable to NULL. The body MUST remove var
 * from list (or leave the loop), or it repeats forever. Evaluates list
 * several times: pass a plain lvalue. */
#define LIST_DRAIN(list, var) \
  while ((list) && (list)->pFirstItem && \
         (((var) = (list)->pFirstItem->pContent), 1))
#endif
