/**
* @file dg_event.c
* This file contains a simplified event system to allow trigedit 
* to use the "wait" command, causing a delay in the middle of a script.
* This system could easily be expanded by coders who wish to implement
* an event driven mud.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
* 
* This source code, which was not part of the CircleMUD legacy code,
* was created by the following people:                                      
* $Author: Mark A. Heilpern/egreen/Welcor $                              
* $Date: 2004/10/11 12:07:00$                                            
* $Revision: 1.0.14 $
*
* Lifetime: event_init() creates the single queue event_q at boot (before
* the first event_create()); event_free_all() frees it and every queued
* event at shutdown and sets event_q to NULL.  Each event is owned by the
* queue while queued and by event_process() while its callback runs; see
* EVENTFUNC in dg_event.h for who frees event_obj.
*
* Input: delays come from builder-written DG 'wait' commands and from
* engine mud events.  Absolute keys (when + pulse) are range-checked here
* because they index the bucket arrays.
*/


#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "dg_event.h"
#include "constants.h"
#include "comm.h"  /* For access to the game pulse */
#include "mud_event.h"

/***************************************************************************
 * Begin mud specific event queue functions
 **************************************************************************/
/* file scope variables */
/** The mud specific queue of events. */
static struct dg_queue *event_q;

/* The event whose func event_process() is calling right now, or NULL.
 * Invariant: a running event has q_el == NULL and is owned by
 * event_process() until func returns.  A callback can reach event_cancel()
 * on its own event through engine code it does not control (for example
 * hit() -> die() -> extract_char() -> clear_char_event_list()), so
 * event_cancel() must not free the running event: it records the request
 * in dg_running_event_cancelled, and event_process() frees the event after
 * func returns without re-enqueueing it. */
static struct event *dg_running_event;
static bool dg_running_event_cancelled;

/** Entity ID for diagnostics: the mud_event_data iId of a mud event, or -1
 * for a DG wait event or an event whose payload is already gone. */
static int event_mud_id(const struct event *event)
{
  if (event == NULL || !event->isMudEvent || event->event_obj == NULL)
    return -1;
  return (int)((const struct mud_event_data *)event->event_obj)->iId;
}

/** Initializes the main event queue event_q.
 * @pre Called once at boot, before the first event_create().
 * @post The main event queue, event_q, has been created and initialized.
 */
void event_init(void)
{
  event_q = queue_init();
}


/** Creates a new event 'object' that is then enqueued to the global event_q.
 * @post If the newly created event is valid, it is always added to event_q.
 * @param func The function to be called when this event fires. This function
 * will be passed event_obj when it fires. The function must match the form
 * described by EVENTFUNC. 
 * @param event_obj An optional 'something' to be passed to func when this
 * event fires. It is func's job to cast event_obj. If event_obj is not needed,
 * pass in NULL.
 * @param when Number of pulses between firing(s) of this event.  Valid
 * range is 1 .. LONG_MAX - pulse; smaller values are clamped to 1 with a
 * WARN (dg/event-when-clamped), larger ones are parked at key LONG_MAX
 * (never fire) with dg/event-schedule-overflow.
 * @pre event_init() has run and event_free_all() has not (otherwise
 * FATAL dg/event-queue-uninit).
 * @retval event * Returns a pointer to the newly created event, never NULL.
 * isMudEvent is FALSE; attach_mud_event() sets it afterwards.  Events due
 * on the same pulse run in reverse insertion order (queue_enq inserts
 * before equal keys).
 **/
/* Queue key for an event due delay pulses from now.  The key indexes the
 * queue buckets (key % NUM_EVENT_QUEUES), so it must never wrap negative;
 * an absurd delay is clamped to the latest representable pulse. */
static long event_key(long delay, const char *caller)
{
  long latest = pulse < (unsigned long)LONG_MAX ? LONG_MAX - (long)pulse : 0;

  if (delay > latest) {
    TB_ERROR_MSG("event", "event-time", caller,
                 "delay of %ld pulses at pulse %lu overflows the queue key; "
                 "clamped to %ld", delay, pulse, latest);
    delay = latest;
  }
  return delay + (long)pulse;
}

struct event *event_create(EVENTFUNC(*func), void *event_obj, long when)
{
  struct event *new_event;

  if (event_q == NULL) {
    /* Before event_init() or after event_free_all().  Callers dereference
     * the result, so there is nothing safe to return. */
    TB_ERROR_LOG("FATAL", "dg", "event-queue-uninit", "event", 0,
                 "event_create(when=%ld) with no event queue", when);
    abort();
  }

  if (when < 1) { /* make sure its in the future */
    /* A DG 'wait 0', 'wait -5' or an 'until' time already past reaches
     * here; the event still runs next pulse (legacy behaviour), but the
     * builder's script asked for something else, so say so. */
    TB_ERROR_LOG("WARN", "dg", "event-when-clamped", "event", 0,
                 "when=%ld is not in the future; clamped to 1 pulse", when);
    when = 1;
  }


  CREATE(new_event, struct event, 1);
  new_event->func = func;
  new_event->event_obj = event_obj;
  new_event->q_el = queue_enq(event_q, new_event,
                              event_key(when, "event_create"));
  new_event->isMudEvent = FALSE;

  return new_event;
}

/** Removes an event from event_q and frees the event and its event_obj.
 * Called through the event_cancel() macro so diagnostics name the caller.
 * @pre event is non-NULL and either queued or the event whose callback is
 * running (dg/event-cancel-null / dg/event-cancel-unqueued otherwise).
 * @post A queued event and its event_obj are freed.  A running event has
 * its event_obj freed now and the struct freed by event_process() when the
 * callback returns.  Either way the caller must clear every pointer it
 * holds to the event (GET_TRIG_WAIT, mud event list entries).
 * @param event Pointer to the event to be dequeued and removed.
 * @param file Caller's __FILE__.
 * @param line Caller's __LINE__.
 * @param func Caller's __func__.
 */
void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func)
{
  if (!event) {
    tb_error_log_at("ERROR", "dg", "event-cancel-null", "event", 0,
                    file, line, func, "attempted to cancel a NULL event");
    return;
  }

  if (event_q == NULL) {
    /* After event_free_all() every queued event is already freed, so the
     * pointer must not be read; leaking is the only safe outcome. */
    tb_error_log_at("ERROR", "dg", "event-queue-uninit", "event", 0,
                    file, line, func,
                    "cancel of event %p with no event queue; not touched",
                    (void *)event);
    return;
  }

  if (!EVENT_IS_QUEUED(event) && event == dg_running_event) {
    /* Cancelling the event whose callback is on the stack: release the
     * payload now (a mud event must leave its owner's list before the owner
     * is freed) but leave the event struct to event_process(). */
    if (dg_running_event_cancelled) {
      tb_error_log_at("ERROR", "dg", "event-cancel-running-twice", "event", 0,
                      file, line, func,
                      "running event %p (mud=%d) cancelled again; ignored",
                      (void *)event, (int)event->isMudEvent);
      return;
    }
    tb_error_log_at("WARN", "dg", "event-cancel-running", "event", 0,
                    file, line, func,
                    "event %p (mud=%d mud_id=%d obj=%p) cancelled from inside "
                    "its own callback; payload freed now, event freed after "
                    "the callback returns and not re-enqueued",
                    (void *)event, (int)event->isMudEvent,
                    event_mud_id(event), event->event_obj);
    if (event->event_obj)
      cleanup_event_obj(event);
    event->event_obj = NULL;
    dg_running_event_cancelled = TRUE;
    return;
  }

  if (!EVENT_IS_QUEUED(event)) {
    /* Every event_create()d event is queued unless it is running, so this
     * is a stale or foreign pointer.  Legacy behaviour frees it anyway. */
    tb_error_log_at("ERROR", "dg", "event-cancel-unqueued", "event", 0,
                    file, line, func,
                    "event %p (mud=%d mud_id=%d) is neither queued nor "
                    "running; freeing anyway",
                    (void *)event, (int)event->isMudEvent,
                    event_mud_id(event));
  } else
    queue_deq(event_q, event->q_el);

  if (event->event_obj)
      cleanup_event_obj(event);

  free(event);
}

/* The memory freeing routine tied into the mud event system */
void cleanup_event_obj(struct event *event)
{
  struct mud_event_data * mud_event;

  if (event->isMudEvent) {  
    mud_event = (struct mud_event_data *) event->event_obj;
    free_mud_event(mud_event);
  } else
    free(event->event_obj);
}

/** Process any events whose time has come. Should be called from, and at, every
 * pulse of heartbeat. Re-enqueues multi-use events.
 */
void event_process(void)
{
  struct event *the_event;
  long new_time;

  if (event_q == NULL) {
    TB_ERROR_MSG("dg", "event-queue-uninit", "event",
                 "event_process at pulse %lu with no event queue", pulse);
    return;
  }

  while ((long) pulse >= queue_key(event_q)) {
    if (!(the_event = (struct event *) queue_head(event_q))) {
      /* queue_key() saw a head in this pulse's bucket, so queue_head()
       * must return it; NULL means the queue holds a NULL data pointer. */
      TB_ERROR_MSG("dg", "event-queue-null-head", "event",
                   "queue head for pulse %lu is NULL; stopping this pulse",
                   pulse);
      return;
    }

    /* Set the_event->q_el to NULL so that any functions called beneath 
     * event_process can tell if they're being called beneath the actual
     * event function. */
    the_event->q_el = NULL;

    /* call event func, reenqueue event if retval > 0 */
    dg_running_event = the_event;
    dg_running_event_cancelled = FALSE;
    new_time = (the_event->func)(the_event->event_obj);
    dg_running_event = NULL;

    if (dg_running_event_cancelled) {
      /* event_cancel() already released event_obj; only the struct is
       * left, and it must not run again whatever func returned. */
      dg_running_event_cancelled = FALSE;
      free(the_event);
    } else if (new_time > 0)
      the_event->q_el = queue_enq(event_q, the_event,
                                  event_key(new_time, "event_process"));
    else
    {
      if (the_event->isMudEvent && the_event->event_obj != NULL)
        free_mud_event((struct mud_event_data *) the_event->event_obj);
      /* It is assumed that the_event will already have freed ->event_obj. */
      free(the_event);
    }
      
  }
}

/** Returns the time remaining before the event as how many pulses from now. 
 * @param event Check this event for its scheduled activation time.
 * @retval long Number of pulses before this event will fire. */
long event_time(struct event *event)
{
  long when;

  when = queue_elmt_key(event->q_el);

  return (when - pulse);
}

/** Frees all events from event_q, their event_obj payloads, and the queue.
 * Shutdown only (destroy_db).
 * @pre Every owner (chars, objs, rooms, descriptors) has already cancelled
 * its mud events, because free_mud_event() edits the owner's event list;
 * no GET_TRIG_WAIT() pointer is used afterwards.
 * @post event_q is NULL; later create/cancel/process calls report
 * dg/event-queue-uninit.  Safe to call more than once. */
void event_free_all(void)
{
  if (event_q != NULL)
    queue_free(event_q);
  /* Every queued event was freed with the queue; NULL makes any later
   * create/cancel/process report event-queue-uninit instead of touching
   * freed memory. */
  event_q = NULL;
}

/** Boolean function to tell whether an event is queued or not. Does this by
 * checking if event->q_el points to anything but null.
 * @retval int 1 if the event has been queued, 0 if the event has not.
 **/
int event_is_queued(struct event *event)
{
   if (EVENT_IS_QUEUED(event))
     return 1;
   else
     return 0;
}
/***************************************************************************
 * End mud specific event queue functions
 **************************************************************************/

/***************************************************************************
 * Begin generic (abstract) priority queue functions
 **************************************************************************/
/** Bucket index for a queue key.
 * Invariant: keys are non-negative (event_create() and event_process()
 * reject overflowing keys).  A negative key would index head[]/tail[] out
 * of bounds, a corruption Valgrind cannot see because the neighbouring
 * memory is addressable, so stop the process instead. */
static int event_bucket(long key)
{
  if (key < 0) {
    TB_ERROR_LOG("FATAL", "dg", "event-bucket-negative", "queue", 0,
                 "queue key %ld is negative; bucket index would be out of "
                 "bounds", key);
    abort();
  }
  return (int)(key % NUM_EVENT_QUEUES);
}

/** Create a new, empty, priority queue and return it.
 * @retval dg_queue * Pointer to the newly created queue structure. */
struct dg_queue *queue_init(void)
{
  struct dg_queue *q;

  CREATE(q, struct dg_queue, 1);

  return q;
}

/** Add some 'data' to a priority queue. 
 * @pre The paremeter q must have been previously created by queue_init.
 * @post A new q_element is created to hold the data parameter.
 * @param q The existing dg_queue to add an element to. 
 * @param data The data to be associated with, and theoretically used, when
 * the element comes up in q. data is wrapped in a new q_element.
 * @param key Indicates where this event should be located in the queue, and
 * when the element should be activated.  Must be >= 0: it selects the
 * bucket (FATAL dg/event-bucket-negative otherwise).  The new element goes
 * before existing elements with an equal key.
 * @retval q_element Pointer to the created q_element that contains
 * the data. */
struct q_element *queue_enq(struct dg_queue *q, void *data, long key)
{
  struct q_element *qe, *i;
  int bucket;

  CREATE(qe, struct q_element, 1);
  qe->data = data;
  qe->key = key;

  bucket = event_bucket(key);   /* which queue does this go in */

  if (!q->head[bucket]) { /* queue is empty */
    q->head[bucket] = qe;
    q->tail[bucket] = qe;
  }

  else {
    for (i = q->tail[bucket]; i; i = i->prev) {

      if (i->key < key) { /* found insertion point */
	if (i == q->tail[bucket])
	  q->tail[bucket] = qe;
	else {
	  qe->next = i->next;
	  i->next->prev = qe;
	}

	qe->prev = i;
	i->next = qe;
	break;
      }
    }

    if (i == NULL) { /* insertion point is front of list */
      qe->next = q->head[bucket];
      q->head[bucket] = qe;
      qe->next->prev = qe;
    }
  }

  return qe;
}

/** Remove queue element qe from the priority queue q.
 * @pre qe->data has been dealt with in some way.
 * @post qe has been freed. 
 * @param q Pointer to the queue containing qe.
 * @param qe Pointer to the q_element to remove from q.
 */
void queue_deq(struct dg_queue *q, struct q_element *qe)
{
  int i;

  assert(qe);

  i = event_bucket(qe->key);

  if (qe->prev == NULL)
    q->head[i] = qe->next;
  else
    qe->prev->next = qe->next;

  if (qe->next == NULL)
    q->tail[i] = qe->prev;
  else
    qe->next->prev = qe->prev;

  free(qe);
}

/** Removes and returns the data of the first element of the priority queue q. 
 * @pre pulse must be defined. This is a multi-headed queue, the current
 * head is determined by the current pulse.
 * @post the q->head is dequeued. 
 * @param q The queue to return the head of.
 * @retval void * NULL if there is not a currently available head, pointer
 * to any data object associated with the queue element. */
void *queue_head(struct dg_queue *q)
{
  void *dg_data;
  int i;

  i = pulse % NUM_EVENT_QUEUES;

  if (!q->head[i])
    return NULL;

  dg_data = q->head[i]->data;
  queue_deq(q, q->head[i]);
  return dg_data;
}

/** Returns the key of the head element of the priority queue.
 * @pre pulse must be defined. This is a multi-headed queue, the current
 * head is determined by the current pulse. 
 * @param q Queue to check for.
 * @retval long Return the key element of the head q_element. If no head
 * q_element is available, return LONG_MAX. */
long queue_key(struct dg_queue *q)
{
  int i;

  i = pulse % NUM_EVENT_QUEUES;

  if (q->head[i])
    return q->head[i]->key;
  else
    return LONG_MAX;
}

/** Returns the key of queue element qe.
 * @param qe Pointer to the keyed q_element.
 * @retval long Key of qe. */
long queue_elmt_key(struct q_element *qe)
{
  return qe->key;
}

/** Free q and all contents.
 * @pre Function requires definition of struct event.  Every element's
 * data is a struct event (or NULL); its event_obj is released through
 * cleanup_event_obj(), so the same owner preconditions as event_free_all()
 * apply.  Callers must not use q afterwards.
 * @post All items associeated qith q, including non-abstract data, are freed.
 * @param q The priority queue to free.
 */
void queue_free(struct dg_queue *q)
{
  int i;
  struct q_element *qe, *next_qe;
  struct event *event;

  for (i = 0; i < NUM_EVENT_QUEUES; i++)
  {
    for (qe = q->head[i]; qe; qe = next_qe) 
    {
      next_qe = qe->next;
      if ((event = (struct event *) qe->data) != NULL) 
      {
        if (event->event_obj)
          cleanup_event_obj(event);

        free(event);
      }
      free(qe);
    }
  }

  free(q);
}

