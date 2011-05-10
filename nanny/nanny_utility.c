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
#include <sys/types.h>
#include <netdb.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "nanny.h"

const char *
nanny_hostname(void)
{
  static char hostname[255];
  struct hostent *he;

  if (hostname[0] == '\0') {
    gethostname(hostname, sizeof(hostname));
    he = gethostbyname(hostname);
    if (he != NULL) {
      strncpy(hostname, he->h_name, sizeof(hostname));
      hostname[sizeof(hostname) - 1] = '\0';
    }
  }
  return hostname;
}

const char *
nanny_username(void)
{
  static char name[64];
  static char *pname;
  struct passwd *pw;
  if (name[0] == '\0') {
    pw = getpwuid(getuid());
    if (pw == NULL) {
      strlcpy(name, "unknown", sizeof(name));
      pname = NULL; /* No name */
    } else {
      strlcpy(name, pw->pw_name, sizeof(name));
      pname = name;
    }
  }
  return pname;
}

const char *
nanny_isotime(time_t t)
{
  static char buff[128];

  if (t == 0)
    t = nanny_globals.now;
  strftime(buff, sizeof(buff), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  return buff;
}
