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
#include <sys/types.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "nanny_timer.h"
#include "nanny.h"

struct nanny_globals_t nanny_globals;


static int last_timer = 0;
static int timer_count = 0;

void t1(void *d, time_t now)
{
  struct timeval tv;
  long err;

  timer_count ++;
  fprintf(stderr, "Timer %d fired: %ld\n", timer_count, (long)now);
  gettimeofday(&tv, NULL);

  err = ((long)tv.tv_sec - (long)now) * 1000000L + (long)tv.tv_usec;
  fprintf(stderr, "  Error: %ld.%06lds\n", err / 1000000, err % 1000000);

  assert(err > 0); /* Timers should never fire early. */
  /* Except for the first two (see below), the timers should not fire
   * more than a few hundred us late. */
  if (timer_count > 2) {
    assert(err < 1000);
  } else if (timer_count > 1) {
    /* Second one could be up to a second late. */
    assert(err < 1000000);
  } else {
    /* First one should be 1-2 seconds late. */
    assert(err < 2000000);
    assert(err > 1000000);
  }

  /* Require that timers go off every second. */
  if (last_timer != 0)
    assert( now == last_timer + 1);
  last_timer = now;
}

int
main(int argc, char **argv)
{
  struct timeval tv;
  struct timer *t;
  time_t now = time(NULL);
  long interval;

  /*
   * Set timers for every second from 1s ago to 10s in the future.
   * The -1s and +0s ones should fire immediately (due to the use of
   * time() for setting, the +0s one will be some large fraction of
   * a second late), and the rest should fire with relatively low
   * error.
   */
  timer_add(now + 10, t1, NULL);
  timer_add(now + 7, t1, NULL);
  timer_add(now + 3, t1, NULL);
  timer_add(now + 1, t1, NULL);
  timer_add(now - 1, t1, NULL);
  timer_add(now + 6, t1, NULL);
  timer_add(now + 5, t1, NULL);
  timer_add(now + 0, t1, NULL);
  timer_add(now + 4, t1, NULL);
  timer_add(now + 2, t1, NULL);
  timer_add(now + 8, t1, NULL);
  timer_add(now + 9, t1, NULL);
  /* Add then remove an extra timer at +4s. */
  t = timer_add(now + 4, t1, NULL);

  timer_delete(t);

  for (;;) {
    timer_next(&tv, NULL);
    assert(timer_count < 13); /* We only set 12 timers; if 13 go off, we lose. */
    fprintf(stderr, "Interval: %ld.%06ld\n",
	    (long int)tv.tv_sec, (long int)tv.tv_usec);
    interval = (long)tv.tv_sec * 1000000 + (long)tv.tv_usec;
    if (timer_count == 12) {
      /* After last timer, make sure we get the 1-hour delay. */
      assert(tv.tv_sec == 3600);
      break;
    }
    /* If this assertion fails, it's because a timer got dropped. */
    assert(tv.tv_sec < 3600);
    if (timer_count > 2) { /* First two are special. */
      /* If this assertion fails, we're screwing up the scheduling. */
      assert(interval > 500000);
      /* If this assertion fails, it took a *long* time to process a timer. */
      assert(interval > 999000);
    }
    /* If this assertion fails, we're screwing up the scheduling. */
    assert(interval < 1000000);
    select(0, NULL, NULL, NULL, &tv);
  }
  return (0);
}
