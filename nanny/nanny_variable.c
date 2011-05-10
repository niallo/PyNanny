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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nanny.h"

/*
 * Return the value of a int parameter.  Returns non-zero if it was found.
 */
static int
nanny_variable_int(const char *key, intmax_t *val)
{
  /*
   * First, try to match one of the predefined values that we can look
   * up from the local environment.
   */
  switch (key[0]) {
  case 'C':
    if (strcmp(key, "CHILD_PID") == 0 && nanny_globals.child_pid > 0) {
      *val = nanny_globals.child_pid;
      return 1;
    }
  case 'G':
    if (strcmp(key, "GID") == 0) {
      *val = getgid();
      return 1;
    }
  case 'H':
    if (strcmp(key, "HTTP_PORT") == 0 && nanny_globals.http_port > 0) {
      *val = nanny_globals.http_port;
      return 1;
    }
  case 'N':
    if (strcmp(key, "NANNY_PID") == 0 && nanny_globals.nanny_pid > 0) {
      *val = nanny_globals.nanny_pid;
      return 1;
    }
  case 'P':
    if (strcmp(key, "PID") == 0 && nanny_globals.child_pid > 0) {
      *val = nanny_globals.child_pid;
      return 1;
    }
  case 'T':
    if (strcmp(key, "TIME") == 0) {
      *val = nanny_globals.now;
      return 1;
    }
  case 'U':
    if (strcmp(key, "UID") == 0) {
      *val = getuid();
      return 1;
    }
  }
  return 0;
}

/*
 * Return the value of a query parameter as a string.
 */
const char *
nanny_variable(const char *key)
{
  static char buff[256];
  intmax_t iv;
  const char *v;

  /*
   * First, look this up as a possible integer-valued local var.
   */
  if (nanny_variable_int(key, &iv)) {
    snprintf(buff, sizeof(buff), "%jd", iv);
    return buff;
  }

  /*
   * Then look it up as a possible string-valued var in the local environment.
   */
  switch (key[0]) {
  case 'H':
    /* HOSTNAME */
    if (strcmp(key, "HOSTNAME") == 0) {
      return nanny_hostname();
    }
  case 'I':
    /* ISOTIME */
    if (strcmp(key, "ISOTIME") == 0) {
      return nanny_isotime(0);
    }
  case 'U':
    /* UNAME */
    if (strcmp(key, "UNAME") == 0 || strcmp(key, "USERNAME") == 0) {
      v = nanny_username();
      return (v == NULL) ? "unknown" : v;
    }
  }

  /* Finally, try to look it up in the Shell environment. */
  v = getenv(key);
  if (v != NULL && v[0] != '\0')
    return v;

  /* If all else fails, return NULL. */
  return NULL;
}

int
nanny_variable_compare(const char *key, const char *value)
{
  intmax_t iv, ref;
  const char *v;

  /* If it's an integer variable, do an integer comparison. */
  if (nanny_variable_int(key, &iv)) {
    ref = strtoll(value, NULL, 10);
    if (ref < iv) return -1;
    if (ref > iv) return 1;
    return 0;
  }

  /* Otherwise, do a string comparison. */
  v = nanny_variable(key);
  if (v == NULL) {
    return -1;
  }
  return strcmp(value, v);
}
