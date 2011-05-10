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
#include <time.h>

/* An opaque reference to a configured timer. */
struct timer;

/*
 * A handler is given a pointer to it's data and the time at which it
 * was scheduled to fire.  Note that the current time may be slightly
 * later than was scheduled.  Timers should use 'now' to compute when
 * to reschedule themselves, to limit the effects of scheduling drift.
 */
typedef void (nanny_timer_handler)(void *, time_t now);

/*
 * Add a new timer.  Note that all timers are one-shots;
 * to run again, you must re-add yourself, typically using
 * code like the following:
 *   void mytimer(void *data, time_t now) {
 *     ... handle timer ...
 *     nanny_timer_add(now + 10, mytimer, data);
 *   }
 */
struct timer *nanny_timer_add(time_t when, nanny_timer_handler *, void *);

/*
 * Returns the time at which the next timer will expire.
 * As a side-effect, all expired timers are serviced.
 *
 * The optional arguments can be used to obtain a more-accurate
 * absolute time and/or an interval until the next expiry.  Any of the
 * pointers can be NULL.  Note that the 'interval' value here is
 * exactly what you need for invoking select(2).
 *
 * If there are no timers, the returned value is arbitrarily set 1hr
 * in the future.
 */
time_t
nanny_timer_next(struct timeval *interval, struct timeval *absolute);

/*
 * Remove a timer.
 */
void
nanny_timer_delete(struct timer *);

/*
 * Query when the timer is scheduled to expire.
 */
time_t
nanny_timer_expiration(struct timer *);
