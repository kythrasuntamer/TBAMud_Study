/**************************************************************************
*  File: lists.c                                           Part of tbaMUD *
*  Usage: Handling of in-game lists                                       *
*                                                                         *
*  By Vatiken. Copyright 2012 by Joseph Arnusch                           *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_event.h"

/* Diagnostics name the caller passed in by the lists.h wrapper macros. */
#define LIST_CALLER_FMT " (caller %s:%d %s)"
#define LIST_CALLER     (file), (line), (func)

/* simple_list() cursor: one global, non-reentrant iterator. Iterator.pItem
 * is the NEXT item simple_list() will hand out (prefetched), so the loop
 * body may remove the item it was just given; unlink_item() advances the
 * cursor if the body removes the next one, and free_list() drops the
 * cursor if the body frees the list. Only this cursor gets that care:
 * merge_iterator() iterators belong to their callers. */
static struct iterator_data Iterator;
static bool loop = FALSE;
static struct list_data *pLastList = NULL;

/* Unlink pItem from pList and free it (not its content). */
static void unlink_item(struct list_data *pList, struct item_data *pItem)
{
  if (loop && pLastList == pList && Iterator.pItem == pItem)
    Iterator.pItem = pItem->pNextItem;

  if (pItem == pList->pFirstItem)
    pList->pFirstItem = pItem->pNextItem;

  if (pItem == pList->pLastItem)
    pList->pLastItem = pItem->pPrevItem;

  if (pItem->pPrevItem)
    pItem->pPrevItem->pNextItem = pItem->pNextItem;

  if (pItem->pNextItem)
    pItem->pNextItem->pPrevItem = pItem->pPrevItem;

  pList->iSize--;
  if (pList->iSize == 0) {
    pList->pFirstItem = NULL;
    pList->pLastItem  = NULL;
  }
  free(pItem);
}

/* Global lists */
struct list_data * global_lists = NULL;
struct list_data * group_list   = NULL;

struct list_data * create_list(void) 
{
  struct list_data *pNewList;
  static bool first_list = TRUE;
  
  CREATE(pNewList, struct list_data, 1);
  
  pNewList->pFirstItem = NULL;
  pNewList->pLastItem  = NULL;
  pNewList->iIterators = 0;
  pNewList->iSize      = 0;
  
  /* Add to global lists, primarily for debugging purposes */
  if (first_list == FALSE)
    add_to_list(pNewList, global_lists);
  else 
    first_list = FALSE;

  return (pNewList);
}

static struct item_data * create_item(void)
{
  struct item_data *pNewItem;

  CREATE(pNewItem, struct item_data, 1);

  pNewItem->pNextItem = NULL;
  pNewItem->pPrevItem = NULL;
  pNewItem->pContent  = NULL;

  return (pNewItem);
}

void free_list_at(struct list_data * pList,
                  const char *file, int line, const char *func)
{
  /* No caller owns a NULL list; report the call rather than crash. */
  if (pList == NULL) {
    TB_ERROR_MSG("lists", "free-null", "list_data",
                 "free_list called with NULL" LIST_CALLER_FMT, LIST_CALLER);
    return;
  }

  /* A simple_list() cursor on this list dies with it (e.g. an event cancel
   * loop whose last cancel frees the owner's list). */
  if (Iterator.pList == pList || (loop && pLastList == pList))
    clear_simple_list();
  /* Freeing a non-empty list is normal (the contents are not owned by the
   * list): drain it from the head. This used to iterate with simple_list(),
   * which remove_from_list() reset after the first item, leaking the rest. */
  while (pList->pFirstItem)
    unlink_item(pList, pList->pFirstItem);

  /* Invariant: the item chain and the counters agree, and no caller still
   * iterates the list. */
  if (pList->iSize != 0 || pList->iIterators != 0)
    TB_ERROR_MSG("lists", "free-nonempty", "list_data",
                 "list %p freed with size %u and %u live iterators after draining"
                 LIST_CALLER_FMT, (void *) pList, (unsigned) pList->iSize,
                 (unsigned) pList->iIterators, LIST_CALLER);

  /* Global List for debugging */
  if (pList != global_lists)
    remove_from_list_at(pList, global_lists, file, line, func);

  free(pList);
}

void add_to_list_at(void * pContent, struct list_data * pList,
                    const char *file, int line, const char *func)
{
  struct item_data * pNewItem;
  struct item_data * pLastItem;

  if (pList == NULL) {
    TB_ERROR_MSG("lists", "add-null", "list_data",
                 "add_to_list content %p to NULL list; not added" LIST_CALLER_FMT,
                 pContent, LIST_CALLER);
    return;
  }

  /* Allocate our memory */
  pNewItem = create_item();

  /* Place the contents in the item */
  pNewItem->pContent  = pContent;
  pNewItem->pNextItem = NULL;

  /* If we are the first entry in the list, mark us as such */
  if (pList->pFirstItem == NULL)
    pList->pFirstItem = pNewItem;
 
  /* Grab our last item from the list and attach it to our new item */
  if (pList->pLastItem) {
    pLastItem = pList->pLastItem;
    pLastItem->pNextItem = pNewItem;
    pNewItem->pPrevItem = pLastItem;
  }

  /* Make our new item our last item in the list */
  pList->pLastItem = pNewItem;

  pList->iSize++;
}

void remove_from_list_at(void * pContent, struct list_data * pList,
                         const char *file, int line, const char *func)
{
  struct item_data *pRemovedItem;

  if ((pRemovedItem = find_in_list(pContent, pList)) == NULL) {
    TB_ERROR_MSG("lists", "remove-missing", "list_data",
                 "content %p is not in list %p (size %u)" LIST_CALLER_FMT,
                 pContent, (void *) pList, pList ? (unsigned) pList->iSize : 0U,
                 LIST_CALLER);
    return;
  }

  unlink_item(pList, pRemovedItem);
}

/** Merges an iterator with a list
 * @post Don't forget to remove the iterator with remove_iterator().
 * */

void * merge_iterator_at(struct iterator_data * pIterator, struct list_data * pList,
                         const char *file, int line, const char *func)
{
  void * pContent;

  if (pList == NULL) {
    TB_WARN_MSG("lists", "iterate-null", "list_data",
                "merge_iterator on a NULL list" LIST_CALLER_FMT, LIST_CALLER);
    pIterator->pList = NULL;
    pIterator->pItem = NULL;
    return NULL;
  }
  if (pList->pFirstItem == NULL) {
    /* Callers check iSize first; an empty list here means they did not. */
    TB_WARN_MSG("lists", "iterate-empty", "list_data",
                "merge_iterator on empty list %p" LIST_CALLER_FMT,
                (void *) pList, LIST_CALLER);
    pIterator->pList = NULL;
    pIterator->pItem = NULL;
    return NULL;
  }

  pList->iIterators++;
  pIterator->pList = pList;
  pIterator->pItem = pList->pFirstItem;

  pContent = pIterator->pItem ? pIterator->pItem->pContent : NULL;

  return (pContent);
}

void remove_iterator_at(struct iterator_data * pIterator,
                        const char *file, int line, const char *func)
{
  if (pIterator->pList == NULL) {
    TB_WARN_MSG("lists", "iterator-release-null", "list_data",
                "remove_iterator on an iterator with no list" LIST_CALLER_FMT,
                LIST_CALLER);
    return;
  }

  pIterator->pList->iIterators--;
  pIterator->pList = NULL;
  pIterator->pItem = NULL;
}

/** Spits out an item and cycles down the list  
 * @return Returns the content of the list
 * */

void * next_in_list_at(struct iterator_data * pIterator,
                       const char *file, int line, const char *func)
{
  void * pContent;
  struct item_data * pTempItem;

  if (pIterator->pList == NULL) {
    TB_WARN_MSG("lists", "iterate-null", "list_data",
                "next_in_list on an iterator with no list" LIST_CALLER_FMT,
                LIST_CALLER);
    return NULL;
  }

  /* Already past the end: stay there. */
  if (pIterator->pItem == NULL)
    return NULL;

  /* Cycle down the list */
  pTempItem = pIterator->pItem->pNextItem;
  pIterator->pItem = pTempItem;

  /* Grab the content */
  pContent = pIterator->pItem ? pIterator->pItem->pContent : NULL;

  return (pContent);
}

/** Searches through the a list and returns the item block that holds pContent
 * @return Returns the actual item block and not the pContent itself, since
 * it is assumed you already have the pContent.
 * */

struct item_data * find_in_list(void * pContent, struct list_data * pList)
{
  struct item_data *pItem;

  /* A plain walk: this used to borrow simple_list()'s global Iterator, so
   * every remove_from_list() inside a simple_list() loop ended that loop. */
  if (pList == NULL)
    return NULL;
  for (pItem = pList->pFirstItem; pItem; pItem = pItem->pNextItem)
    if (pItem->pContent == pContent)
      return pItem;
  return NULL;
}

void clear_simple_list(void)
{
  /* Release a cursor abandoned mid-list (a loop that broke out early). */
  if (Iterator.pList)
    remove_iterator(&Iterator);
  Iterator.pList = NULL;
  Iterator.pItem = NULL;
  loop = FALSE;
  pLastList = NULL;
}

void * simple_list_at(struct list_data * pList,
                      const char *file, int line, const char *func)
{
  struct item_data *pItem;

  /* Reset List */
  if (pList == NULL) {
    clear_simple_list();
    return NULL;
  }

  if (!loop || pLastList != pList) {
    /* Another list's loop was abandoned (or is nested around this one):
     * that loop's cursor is lost. */
    if (loop && pLastList != pList) {
      TB_WARN_MSG("lists", "simple-list-reset", "list_data",
                  "simple_list on list %p abandons the loop over list %p"
                  LIST_CALLER_FMT, (void *) pList, (void *) pLastList,
                  LIST_CALLER);
      clear_simple_list();
    }

    if (merge_iterator_at(&Iterator, pList, file, line, func) == NULL)
      return NULL;
    pLastList = pList;
    loop = TRUE;
  }

  /* Hand out the prefetched item and prefetch its successor before the
   * caller can remove it. */
  if ((pItem = Iterator.pItem) != NULL) {
    Iterator.pItem = pItem->pNextItem;
    return (pItem->pContent);
  }

  remove_iterator(&Iterator);
  loop = FALSE;
  pLastList = NULL;
  return NULL;
}

void * random_from_list(struct list_data * pList)
{
  struct iterator_data localIterator;
  void * pFoundItem;
  bool found;
  int number;
  int count = 1;

  if (pList->iSize <= 0)
    return NULL;
  else
    number = rand_number(1, pList->iSize);

  pFoundItem = merge_iterator(&localIterator, pList);

  for (found = FALSE; pFoundItem != NULL; pFoundItem = next_in_list(&localIterator), count++) {
    if (count == number) {
      found = TRUE;
      break;
    }
  }

  remove_iterator(&localIterator);
  
  if (found)
    return (pFoundItem);
  else
    return NULL;
}

struct list_data * randomize_list(struct list_data * pList)
{
  struct list_data * newList;
  void * pContent;
  
  if (pList->iSize == 0)
    return NULL;
    
  newList = create_list();
  
  while ((pContent = random_from_list(pList)) != NULL) {
    remove_from_list(pContent, pList);
    add_to_list(pContent, newList);
  }
  
  free_list(pList);
  
  return (newList);
}
