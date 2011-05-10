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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nanny.h"
#include "nanny_timer.h"

static int
default_http_page(struct http_request *request)
{
  http_printf(request, "HTTP/1.0 200 OK\x0d\x0a");
  http_printf(request, "Content-Type: text/html\x0d\x0a");
  http_printf(request, "\x0d\x0a");
  http_printf(request, "<HTML>\n");
  http_printf(request, "<head><title>Nanny: %s</title></head>\n",
	      nanny_hostname());
  http_printf(request, "<body>\n");
  http_printf(request, "<ul>\n");
  http_printf(request, "<li>Host: %s\n", nanny_hostname());
  http_printf(request, "<li>Time: %s\n", nanny_isotime(0));
  http_printf(request, "<li><a href=\"/status/\">Children</a><br/>\n");
  http_printf(request, "<li><a href=\"/environment\">Environment</a><br/>\n");
  http_printf(request, "</ul>\n");
  http_printf(request, "</body>\n");
  http_printf(request, "</HTML>\n");
  return (0);
}

/*
 * The 'dispatcher' is invoked after the request line is parsed
 * but before any headers or HTTP body is read.  It can set
 * function callbacks to process header lines and body.
 * The body callback is expected to generate the response.
 */
static void
http_dispatcher(struct http_request *request)
{
#if 0
    printf("Method: %s\n", request->method_name);
    printf("URI: %s\n", request->uri);
    printf("Version: %d.%d\n", request->HTTPmajor, request->HTTPminor);
#endif

    if (strcmp(request->uri, "/environment") == 0) {
      request->body_processor = nanny_http_environ_body;
      return;
    }
    if (strncmp(request->uri, "/status", 7) == 0) {
      request->body_processor = nanny_children_http_status;
      return;
    }
    request->body_processor = default_http_page;
}

/*
 * Once-a-second heartbeat.
 */
void sample_clock(void *d, time_t now)
{
  if ((now % 60) == 0) {
    char buff[64];
    strftime(buff, sizeof(buff), "%H:%M:%S", localtime(&now));
    printf("| %s\n", buff);
  } else if ((now % 30) == 0) printf("X");
  else if ((now % 10) == 0) printf("|");
  else if ((now % 5) == 0) printf(":");
  else printf(".");
  fflush(stdout);
  nanny_timer_add(now + 1, sample_clock, NULL);
}

/*
 * This is probably unnecessary.
 */
static void clean_environment(void)
{
  extern char **environ;
  char *safe[] = { "HOME", "PATH", "PWD", "USER", NULL };
  char **p, *q, *t;
  int i, removed, preserve;

  do {
    removed = 0;
    for (p = environ; *p != NULL && !removed; p++) {
      q = *p;
      while (*q != '\0' && *q != '=')
	++q;
      if (q == *p)
	continue;
      t = malloc(q - *p + 1);
      memcpy(t, *p, q - *p);
      t[q - *p] = '\0';
      preserve = 0;
      for (i = 0; safe[i] != NULL; ++i) {
	if (strcmp(t, safe[i]) == 0) {
	  preserve = 1;
	  break;
	}
      }
      if (!preserve) {
	unsetenv(t);
	removed = 1;
      }
      free(t);
    }
  } while (removed);

}

/* Running flag:  Reset by signal handler, main loop uses this to exit. */
static int running = 1;

void stophandler(int s)
{
  running = 0;
}

static void
nanny_usage(const char *prog)
{
  printf("Usage: %s -s <start_cmd> [options]\n", prog);
  printf(" -d               Debug\n");
  printf(" -h <shell cmd>   Health check\n");
  printf(" -S <shell cmd>   Stop command\n");
  printf(" -t <timed cmd>   Timed command\n");
  printf("Example:\n");
  printf("  %s -s 'bin/server --no-background' -t '8h bin/reset $PID'\n", prog);
  printf("Note: start command must come first\n");
}

int
main(int argc, char **argv)
{
  struct timeval tv;
  struct nanny_child *child = NULL;
  int ch;
  const char *start, *stop, *health;
  struct sigaction sa;
  void *counter;
  int debug = 0;

  /* Parse options. */
  health = start = stop = NULL;
  while ((ch = getopt(argc, argv, "dh:S:s:t:")) != -1) {
    switch (ch) {
    case 'd':
      debug = 1;
      break;
    case 'h':
      nanny_child_set_health(child, optarg);
      break;
    case 'S':
      nanny_child_set_stop(child, optarg);
      break;
    case 's':
      child = nanny_child_new(optarg);
      nanny_child_set_restartable(child, 1);
      break;
    case 't':
      nanny_child_add_periodic(child, optarg);
      break;
    default:
      nanny_usage(argv[0]);
      exit(1);
    }
  }


  /* Register the child. */
  if (child == NULL) {
    nanny_usage(argv[0]);
    return (1);
  }

  nanny_child_set_logpath(child, "/tmp");

  if (!debug)
    nanny_daemonize(NULL);

  nanny_globals.nanny_pid = getpid();

  clean_environment();

  /* We need to handle shutdown requests carefully to ensure that our
   * children are properly signalled. */
  sa.sa_handler = stophandler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  /* Create a UDP server bound to a well-known multicast socket. */
  udp_server_init("226.1.1.1", 8889);
  /* Ditto, using an anonymous unicast socket. */
  /* Note: udp_announce() will use this unicast socket. */
  udp_server_init(NULL, -1);
  /* Create an HTTP server on an anonymous socket using http_dispatcher. */
  http_server_init(NULL, 0, http_dispatcher);
  /* Create a counter server listening on a dynamic Unix socket. */
  counter = nanny_counter_server_init(NULL);

  /* Register a sample timed event. */
  nanny_timer_add(0, sample_clock, NULL);

  /* Announce our HTTP port to multicast group (sent from unicast socket). */
  udp_announce("HTTP_PORT=%d", nanny_globals.http_port);
  printf("HTTP_PORT=%d\n", nanny_globals.http_port);

  /* Alternately process timers and wait for network activity. */
  while (running) {
    /* Handle any sigchld events that may have occurred. */
    nanny_oversee_children();
    /* Service timers and compute delay until the next timer expires. */
    nanny_timer_next(&tv, NULL);
    /* Wait for network activity, a timeout, or a signal. */
    nanny_select(&tv);
  }

  printf("Stop signal received\n");

  /* We're no longer running, so start shutting things down. */
  nanny_counter_server_close(counter);

  /* Same loop as above, but with a different exit condition. */
  while (nanny_stop_all_children()) {
    nanny_oversee_children();
    nanny_timer_next(&tv, NULL);
    nanny_select(&tv);
  }

  printf("\n");
  return (0);
}
