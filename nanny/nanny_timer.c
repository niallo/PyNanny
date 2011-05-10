/*-
 * Copyright (c) 2009 Metaweb Technologies, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Metaweb Technologies nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDES AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/time.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "nanny.h"
#include "nanny_timer.h"

/*
 * Our internal structure for storing a timer.
 * We could convert time_t to struct timeval here if
 * we wanted to track higher-resolution timers, but for
 * my current requirements, 1s resolution is more than
 * sufficient.
 */
struct timer {
  time_t when;
  void *data;
  void (*f)(void *, time_t);
};

time_t
nanny_timer_expiration(struct timer *t)
{
  return (t->when);
}


/*
 * This array is really a heap.  See below.
 */
#define MAX_TIMERS 1024
struct timer *timers[MAX_TIMERS];
int nanny_timer_count = 0;

/* This macro formulation both forces a trailing semicolon (makes it
 * more function like) and provides a code block in which you can
 * declare the temporary.
 */
#define SWAP_TIMERS(a, b) do {struct timer *_t; _t = timers[a]; timers[a] = timers[b]; timers[b] = _t;} while(0)

/*
 * The timer at position 'i' has changed; float it up or down the
 * tree to the right position.  This routine enforces a standard
 * heap invariant assuming that element 'i' is the only element that's
 * not already in the correct place.  This is O(lg n).
 */
static void
nanny_timer_adjust_position(int i)
{
  if (i > 0 && timers[i]->when < timers[(i - 1)/2]->when) {
    /* If we're earlier than our parent, we float up in the tree. */
    int j = (i - 1) / 2;
    while (i > j && timers[i]->when < timers[j]->when) {
      SWAP_TIMERS(i, j);
      i = j;
      j = (i - 1) / 2;
    }
  } else {
    /* We're not earlier than our parent, so float down. */
    while (i < nanny_timer_count) {
      int a = i * 2 + 1;
      int b = i * 2 + 2;
      int min = i;

      if (a < nanny_timer_count && timers[a]->when < timers[min]->when)
	min = a;
      if (b < nanny_timer_count && timers[b]->when < timers[min]->when)
	min = b;
      if (min == i)
	break;
      /* Swap */
      SWAP_TIMERS(min, i);
      i = min;
    }
  }
}

static void
nanny_timer_remove(int i)
{
  /* Do right thing if there are no timers. */
  if (nanny_timer_count < 1 || i > nanny_timer_count)
    return;
  /* Release dead entry. */
  free(timers[i]);
  /* Overwrite timer 'i' with last timer in heap. */
  --nanny_timer_count;
  timers[i] = timers[nanny_timer_count];
  timers[nanny_timer_count] = NULL;
  /* Float it into position. */
  if (i < nanny_timer_count)
    nanny_timer_adjust_position(i);
}

void
nanny_timer_delete(struct timer *t)
{
  int i;
  if (t == NULL)
    return;
  for (i = 0; i < nanny_timer_count; ++i) {
    if (timers[i] == t) {
      nanny_timer_remove(i);
      return;
    }
  }
  /* Didn't find the timer in the array? */
}

struct timer *
nanny_timer_add(time_t when, nanny_timer_handler *f, void *data)
{
  struct timer *t;

  /* Allocate a new timer. */
  t = malloc(sizeof(*t));
  assert(t != NULL);
  t->when = when;
  t->data = data;
  t->f = f;

  /* Add timer at end and float it into the right place in the tree. */
  /* TODO: Automatically resize the array (double it) when we hit the end. */
  nanny_timer_count++;
  assert(nanny_timer_count < MAX_TIMERS);
  timers[nanny_timer_count - 1] = t;
  nanny_timer_adjust_position(nanny_timer_count - 1);

  return (t);
}

/*
 * Process any timers that may have expired and then return the
 * time at which the next timer will expire.
 */
time_t
nanny_timer_next(struct timeval *interval, struct timeval *absolute)
{
  struct timeval now;

  gettimeofday(&now, NULL);
  nanny_globals.now = now.tv_sec;

  while (nanny_timer_count > 0 && now.tv_sec >= timers[0]->when) {
    /* Remove the timer entry before we invoke it, so
     * it can re-register without any conflicts. */
    time_t when = timers[0]->when;
    void *data = timers[0]->data;
    void (*f)(void *, time_t) = timers[0]->f;
    nanny_timer_remove(0);
    /* A zero value for 'when' is just a shorthand for "now". */
    if (when == 0)
      when = now.tv_sec;
    f(data, when);
  }

  /* If no timers remain, just set the response arbitrarily to 1s from now. */
  if (nanny_timer_count < 1) {
    if (interval != NULL) {
      interval->tv_sec = 1;
      interval->tv_usec = 0;
    }
    if (absolute != NULL) {
      absolute->tv_sec = now.tv_sec + 3600;
      absolute->tv_usec = now.tv_usec;
    }
    return (now.tv_sec + 3600);
  }

  /* There are timers, so return the appropriate values. */
  if (absolute != NULL) {
    absolute->tv_sec = timers[0]->when;
    absolute->tv_usec = 0;
  }

  /* We always compute the interval until the next whole second. */
  if (interval != NULL) {
    interval->tv_sec = timers[0]->when - now.tv_sec;
    interval->tv_usec = -now.tv_usec;
    /* Rationalize the return: ensure 0 <= usec < 1000000. */
    while (interval->tv_usec > 999999) {
      interval->tv_usec -= 1000000;
    }
    while (interval->tv_usec < 0) {
      interval->tv_usec += 1000000;
    }
    /* Ensure we don't return a negative interval. */
    if (interval->tv_sec < 0) {
      interval->tv_sec = 0;
    }
    /* Ensure we don't return a zero interval. */
    if (interval->tv_sec == 0 && interval->tv_usec < 1)
      interval->tv_usec = 1;
  }
  /* Always clip interval->tv_sec to 1 s, to avoid potentially long
     delays in processing.  See ENG-509 for details. */
  if (interval->tv_sec > 1)
    interval->tv_sec = 1;
  /*
   * Note: It is entirely possible for the return value here to be in
   * the past.  Caveat consumer.
   */
  return (timers[0]->when);
}
