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
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nanny.h"
#include "nanny_timer.h"

/*
 * Globals
 */
struct nanny_globals_t nanny_globals;

struct {
  int socket;
  void (*handler)(void *);
  void *data;
} listener[512];

void
nanny_select(struct timeval *tv)
{
  fd_set readfds;
  int limit = 0;
  int r;
  size_t i;

  FD_ZERO(&readfds);
  for (i = 0; i < sizeof(listener)/sizeof(listener[0]); ++i) {
    if (listener[i].socket > 0) {
      FD_SET(listener[i].socket, &readfds);
      if (listener[i].socket > limit)
	limit = listener[i].socket + 1;
    }
  }
  r = select(limit, &readfds, NULL, NULL, tv);
  nanny_globals.now = time(NULL);

  if (r < 0) {
    if (errno != EINTR)
      perror("select() failed");
    return;
  }
  if (r == 0)
    return;

  for (i = 0; i < sizeof(listener)/sizeof(listener[0]); ++i) {
    if (listener[i].socket > 0
	&& FD_ISSET(listener[i].socket, &readfds)) {
      /* printf("Data ready on fd %d\n", listener[i].socket); */
      listener[i].handler(listener[i].data);
    }
  }
}

void
nanny_unregister_server(int fd)
{
  size_t i;

  for (i = 0; i < sizeof(listener)/sizeof(listener[0]); ++i) {
    if (listener[i].socket == fd) {
      listener[i].handler = NULL;
      listener[i].data = NULL;
      listener[i].socket = 0;
    }
  }
}

void
nanny_register_server(void (*handler)(void *), int s, void *data)
{
  size_t i;
  for (i = 0; i < sizeof(listener)/sizeof(listener[0]); ++i) {
    if (listener[i].socket == s) {
      fprintf(stderr, "INTERNAL ERROR: Re-registering server on fd %d\n", s);
      listener[i].handler = NULL;
      listener[i].data = NULL;
      listener[i].socket = 0;
      break;
    }
  }
  for (i = 0; i < sizeof(listener)/sizeof(listener[0]); ++i) {
    if (listener[i].handler == NULL) {
      listener[i].handler = handler;
      listener[i].socket = s;
      listener[i].data = data;
      return;
    }
  }
  fprintf(stderr, "INTERNAL ERROR: Ran out of slots for fd handlers.\n");
}


/*
 * Copied and mangled from several examples.
 */
void
nanny_daemonize(const char *pidfile)
{
    pid_t pid, sid;
    int i;

    /* Fork and have the parent exit immediately. */
    pid = fork();
    if (pid < 0)
      exit(1);
    if (pid > 0)
      _exit(0);

    /* At this point we are executing as the child process */

    /* Create a new SID for the child process.  On modern Unix
     * systems, this detaches us from a controlling terminal, which
     * means we can't get ^C, for instance and won't be sent SIGHUP
     * when the person who started us logs out. */
    sid = setsid();
    if (sid < 0)
      exit(1);

    /* Close all file descriptors and re-open stdio to /dev/null. */
    for (i = getdtablesize(); i >= 0; --i)
      close(i);
    i = open("/dev/null", O_RDWR);
    dup(i);
    dup(i);

    /* Sanitize the umask. */
    umask(027);
    /* Set the current directory.  Without this, our old current
     * directory can't be removed, for instance, because we're still
     * referencing it.  The current dir is also the default on many
     * systems for core dumps; change this if you want core dumps to
     * be possible. */
    if ((chdir("/")) < 0)
        exit(1);

    /* Some old Unix systems require another fork() to fully disassociate
     * from the terminal.  This also means that we will no longer be
     * the leader of our session group, which makes it harder to "accidentally"
     * acquire a controlling terminal. */
    pid = fork();
    if (pid < 0)
      exit(1);
    if (pid > 0)
      _exit(0);

    /* At this point we are executing as the grandchild process.  Both
     * our parent and grandparent have exited and the kernel has
     * re-parented us to the init process. */

    /* Save a PID file if we were asked. */
    if (pidfile != NULL) {
      char buff[32];
      int f = open(pidfile, O_RDWR|O_CREAT, 0644);
      if (f < 0)
	exit(1);
      if (lockf(f, F_TLOCK, 0) < 0)
	exit(0);
      snprintf(buff, sizeof(buff), "%d\n", getpid());
      write(f, buff, strlen(buff));
    }
}
