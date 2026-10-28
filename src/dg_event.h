/**
* @file dg_event.h
* This file contains defines for the simplified event system to allow trigedit 
* to use the "wait" command, causing a delay in the middle of a script.
* This system could easily be expanded by coders who wish to implement
* an event driven mud.
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
* 
* This source code, which was not part of the CircleMUD legacy code,
* is attributed to:                                      
* $Author: Mark A. Heilpern/egreen/Welcor $                              
* $Date: 2004/10/11 12:07:00$                                            
* $Revision: 1.0.14 $
*
* Who controls the input: event delays come from DG 'wait' arguments
* (builder-written scripts, via process_wait in dg_scripts.c) and from mud
* event callbacks (engine code).  Delays are relative pulse counts; the
* queue stores absolute keys (when + pulse), which must be non-negative.
* Ownership rules for events and their payloads are documented at
* EVENTFUNC below.
*/
#ifndef TBAMUD_DG_EVENT_H_INCLUDED
#define TBAMUD_DG_EVENT_H_INCLUDED

/** How often will heartbeat() call the 'wait' event function?
 * @deprecated Currently not used. */
#define PULSE_DG_EVENT 1

/**************************************************************************
 * Begin event structures and defines.
 **************************************************************************/
/** All Functions handled by the event system must be of this format.
 *
 * Return value: > 0 re-runs the event that many pulses later with the same
 * event_obj; <= 0 ends it.
 *
 * Ownership of event_obj:
 * - Mud events (isMudEvent): the engine owns the mud_event_data and frees
 *   it through free_mud_event() when the callback returns <= 0, when the
 *   event is cancelled, or when a reschedule is rejected.
 * - Other events (DG 'wait'): the callback must free event_obj itself
 *   before returning <= 0, and must not free it when returning > 0.
 *   event_cancel() and event_free_all() free() it if the event never ran.
 *
 * A callback may cause its own event to be cancelled (for example a hit()
 * that kills a character whose events are then cleared).  The payload is
 * freed at that point, so the callback must not touch event_obj after
 * calling anything that can extract its owner; the event struct is freed
 * after the callback returns and is never re-run. */
#define EVENTFUNC(name) long (name)(void *event_obj)

/** The event structure. Events get attached to the queue and are executed
 * when their turn comes up in the queue. Allocated by event_create() and
 * freed only by dg_event.c (event_process, event_cancel, event_free_all);
 * holders such as GET_TRIG_WAIT() and the mud event lists must drop their
 * pointer when the event ends or is cancelled. */
struct event {
  EVENTFUNC(*func); /**< The function called when this event comes up. */
  void *event_obj;  /**< event_obj is passed to func when func is called */
  /** Where this event is located in the queue.  NULL only while
   * event_process() is running func for this event. */
  struct q_element *q_el;
  bool isMudEvent;  /**< event_obj is a mud_event_data (see EVENTFUNC) */
};

/** TRUE while ev sits in the queue; FALSE while its callback is running
 * (event_process clears q_el first) or for a stale event.  ev is evaluated
 * once. */
#define EVENT_IS_QUEUED(ev)  ((ev)->q_el != NULL)
/**************************************************************************
 * End event structures and defines.
 **************************************************************************/

/**************************************************************************
 * Begin priority queue structures and defines.
 **************************************************************************/
/** Number of buckets available in each queue. Reduces enqueue cost. */
#define NUM_EVENT_QUEUES    10

/** The priority queue. */
struct dg_queue {
  struct q_element *head[NUM_EVENT_QUEUES]; /**< Front of each queue bucket. */
  struct q_element *tail[NUM_EVENT_QUEUES]; /**< Rear of each queue bucket. */
};

/** Queued elements. */
struct q_element {
  void *data;  /**< The event to be handled. */
  long key;    /**< When the event should be handled. */
  struct q_element *prev, *next; /**< Points to other q_elements in line. */
};
/**************************************************************************
 * End priority queue structures and defines.
 **************************************************************************/

/* - events - function protos needed by other modules */
void event_init(void);
struct event *event_create(EVENTFUNC(*func), void *event_obj, long when);
void event_cancel_at(struct event *event, const char *file, int line,
                     const char *func);
/** Cancel and free an event; diagnostics name the calling file/line/func. */
#define event_cancel(ev) event_cancel_at((ev), __FILE__, __LINE__, __func__)
void event_process(void);
long event_time_at(struct event *event, const char *file, int line,
                   const char *func);
/** Pulses until a queued event fires; -1 (reported) for a NULL event or one
 * whose callback is running.  Diagnostics name the calling file/line/func. */
#define event_time(ev) event_time_at((ev), __FILE__, __LINE__, __func__)
void event_free_all(void);
void cleanup_event_obj(struct event *event);

/* - queues - function protos need by other modules */
struct dg_queue *queue_init(void);
struct q_element *queue_enq(struct dg_queue *q, void *data, long key);
void queue_deq(struct dg_queue *q, struct q_element *qe);
void *queue_head(struct dg_queue *q);
long queue_key(struct dg_queue *q);
long queue_elmt_key(struct q_element *qe);
void queue_free(struct dg_queue *q);
int  event_is_queued_at(struct event *event, const char *file, int line,
                        const char *func);
/** 1 if ev is queued, 0 if its callback is running or ev is NULL (the NULL
 * case is reported naming the caller). */
#define event_is_queued(ev) event_is_queued_at((ev), __FILE__, __LINE__, __func__)

#endif /* TBAMUD_DG_EVENT_H_INCLUDED */
