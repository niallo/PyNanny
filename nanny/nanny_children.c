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
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nanny.h"
#include "nanny_timer.h"

/* How often to run the health checks. */
#define HEALTH_PERIOD 60
/* Terminate health check (with failure) if it runs longer than this. */
#define HEALTH_TIMEOUT 60
/* Default buffer length for environment variables */
#define ENVLEN 64

/*
 * States for starting a main child:
 *   "NEW" or "RESTARTING" children are waiting to be started.
 *   "STARTING" has been forked but is still "on probation."  If
 *     it fails during the probationary period, that's counted
 *     as a premature exit.
 *   After the probationary period expires, we're in state "RUNNING."
 *
 * See main_child_goal_running() for more details.
 */
static const char *NEW = "new";
static const char *RESTARTING = "waiting to restart after failure";
static const char *STARTING = "starting (on probation)";
static const char *RUNNING = "running";

/*
 * States for shutting down a process.
 *
 * We first run a custom shell cmd and wait a long time to give that a
 * chance to succeed.  If the program is still running after the stop
 * probation expires, we send increasingly severe signals with
 * decreasing patience.
 *
 * See main_child_goal_stopped() for more details.
 */
#define STOP_PROBATION 300 /* 300 seconds = 5 minutes */
static const char *STOPPING1 = "stopping(custom)"; /* + STOP_PROBATION */
/* If there is no custom stop action, we go straight to SIGTERM, but
 * with a STOP_PROBATION timeout. */
/* Rest of the timeouts are hardcoded below; commented here. */
static const char *STOPPING2 = "stopping(sigterm)"; /* + 15s wait */
static const char *STOPPING3 = "stopping(sigkill)"; /* + 15s wait */
static const char *STOPPED = "stopped";


struct timed_t {
  struct timed_t *next;
  struct timer *timer;
  time_t interval;
  time_t last;
  char *cmd;
  int envplen;
  const char **envp;
};

/* A list of all live children. */
static struct nanny_child *live_children_oldest;
static struct nanny_child *live_children_youngest;

static void
child_free(struct nanny_child *child)
{
  free(child->instance);
  child->instance = NULL;
  free(child->start_cmd);
  child->start_cmd = NULL;
  free(child->stop_cmd);
  child->stop_cmd = NULL;
  free(child->health_cmd);
  child->health_cmd = NULL;
  nanny_timer_delete(child->state_timer);
  child->state_timer = NULL;
  nanny_timer_delete(child->health_timer);
  child->health_timer = NULL;

  nanny_log_release(child->child_stdout);
  nanny_log_release(child->child_stderr);
  nanny_log_release(child->child_events);

  if (child == live_children_oldest)
    live_children_oldest = child->younger;
  if (child == live_children_youngest)
    live_children_youngest = child->older;
  if (child->younger != NULL)
    child->younger->older = child->older;
  if (child->older != NULL)
    child->older->younger = child->younger;

  free(child);
}

static struct nanny_child *
child_alloc(const char *start_cmd)
{
  static int id = 0;
  struct nanny_child *child;

  child = malloc(sizeof(*child));
  memset(child, 0, sizeof(*child));
  child->state = NEW;
  /* If this is the first child, it's also the oldest. */
  if (live_children_oldest == NULL)
    live_children_oldest = child;
  /* This is the new youngest child. */
  child->younger = NULL;
  child->older = live_children_youngest;
  if (child->older != NULL)
    child->older->younger = child;
  live_children_youngest = child;

  child->id = id++;

  /* 'start_cmd' may not be NULL, so don't bother checking. */
  child->start_cmd = strdup(start_cmd);

  return (child);
}

void
nanny_child_set_stop(struct nanny_child *child, const char *cmd)
{
  child->stop_cmd = strdup(cmd);
}

void
nanny_child_set_health(struct nanny_child *child, const char *cmd)
{
  child->health_cmd = strdup(cmd);
}

void
nanny_child_set_restartable(struct nanny_child *child, int flag)
{
  child->restartable = flag;
}


void
nanny_child_set_envp(struct nanny_child *child, const char **envp)
{
  child->envp = envp;
}

/*
 *
 * HTTP RESPONSE GENERATION
 *
 * Functions that generate status pages about the children.
 */

static int
nanny_children_http_child_log(struct http_request *request,
			      struct nanny_child *child,
			      struct nanny_log *iostore,
			      const char *name)
{
  http_printf(request, "HTTP/1.0 200 OK\x0d\x0a");
  http_printf(request, "Content-Type: text/plain\x0d\x0a");
  http_printf(request, "\x0d\x0a");
  http_printf(request, "# %s, child #%d, pid %d, time %s\n",
	      name, child->id, child->pid, nanny_isotime(0));
  nanny_log_http_dump_raw(request, iostore);
  return (0);
}

static int
nanny_children_http_child(struct http_request *request, struct nanny_child *child)
{
  /* Standard HTTP response header. */
  http_printf(request, "HTTP/1.0 200 OK\x0d\x0a");
  http_printf(request, "Content-Type: text/plain\x0d\x0a");
  http_printf(request, "\x0d\x0a");

  http_printf(request, "{\n");
  http_printf(request, " \"time\":\"%s\",\n", nanny_isotime(0));
  http_printf(request, " \"child\":");

  http_printf(request, "  {\n");
  http_printf(request, "   \"id\": %d,\n", child->id);
  http_printf(request, "   \"start_cmd\": \"%s\",\n", child->start_cmd);
  if (child->pid > 0)
    http_printf(request, "   \"pid\": %d,\n", child->pid);
  if (child->instance != NULL)
    http_printf(request, "   \"instance\": \"%s\",\n", child->instance);
  if (child->stop_cmd != NULL)
    http_printf(request, "   \"stop_cmd\": \"%s\",\n", child->stop_cmd);
  if (child->health_cmd != NULL)
    http_printf(request, "   \"health_cmd\": \"%s\",\n", child->health_cmd);
  http_printf(request, "   \"health_failures_consecutive\": %d,\n",
	      child->health_failures_consecutive);
  http_printf(request, "   \"health_failures_total\": %d,\n",
	      child->health_failures_total);
  http_printf(request, "   \"health_successes_consecutive\": %d,\n",
	      child->health_successes_consecutive);
  http_printf(request, "   \"health_successes_total\": %d,\n",
	      child->health_successes_total);
  http_printf(request, "   \"restartable\": %s,\n",
	      child->restartable ? "true" : "false");
  http_printf(request, "   \"state\": \"%s\",\n", child->state);
  http_printf(request, "   \"start_count\": %d\n", child->start_count);
  if (child->last_start > 0)
    http_printf(request, "   \"last_start\": \"%s\",\n",
		nanny_isotime(child->last_start));
  if (child->last_stop > 0)
    http_printf(request, "   \"last_stop\": \"%s\",\n",
		nanny_isotime(child->last_stop));
  if (child->state_timer != NULL)
    http_printf(request, "   \"next_state_check\": \"%s\",\n",
		nanny_isotime(nanny_timer_expiration(child->state_timer)));
  if (child->health_timer != NULL)
    http_printf(request, "   \"next_health_check\": \"%s\",\n",
		nanny_isotime(nanny_timer_expiration(child->health_timer)));

  nanny_log_http_dump_json(request, child->child_stdout, "stdout", "   ");
  nanny_log_http_dump_json(request, child->child_stderr, "stderr", "   ");
  nanny_log_http_dump_json(request, child->child_events, "events", "   ");

  http_printf(request, " }\n");
  http_printf(request, "}\n");
  return (0);

}

/*
 * Summary status for all children.
 */
static int
nanny_children_http(struct http_request *request, const char *prefix)
{
  struct nanny_child *child;

  http_printf(request, "<HTML><HEAD><TITLE>All Children</TITLE></HEAD>\n");
  http_printf(request, "<BODY>\n");
  http_printf(request, "<PRE>\n");
  http_printf(request, "Current time: %s\n", nanny_isotime(0));
  http_printf(request, "<a href=\"http://%s:8123/\">Qbert</a>\n", nanny_hostname());
  http_printf(request, "\n");
  for (child = live_children_oldest; child != NULL; child = child->younger) {
    http_printf(request, "<A HREF=\"%s/%d\">Child %d</A>\n",
		prefix, child->id, child->id);
    if (child->main != NULL)
      http_printf(request, "  subsidiary to: child %d\n", child->main->id);
    if (child->pid > 0)
      http_printf(request, "  pid: %d\n", child->pid);
    if (child->instance != NULL)
      http_printf(request, "  Instance: %s\n", child->instance);
    http_printf(request, "  start cmd: %s\n", child->start_cmd);
    http_printf(request, "  stop cmd: %s\n", child->stop_cmd);
    http_printf(request, "  health cmd: %s\n", child->health_cmd);
    http_printf(request, "  consecutive health failures: %d\n",
		child->health_failures_consecutive);
    http_printf(request, "  restartable: %s\n",
		child->restartable ? "YES" : "NO");
    http_printf(request, "  state: %s\n", child->state);
    http_printf(request, "  start count: %d\n", child->start_count);
    if (child->last_start > 0)
      http_printf(request, "  last start: %s\n",
		  nanny_isotime(child->last_start));
    if (child->last_stop > 0)
      http_printf(request, "  last stop: %s\n",
		  nanny_isotime(child->last_stop));
    http_printf(request, "  <a href=\"%s/%d/stdout\">stdout</a>\n",
		prefix, child->id);
    http_printf(request, "  <a href=\"%s/%d/stderr\">stderr</a>\n",
		prefix, child->id);
    http_printf(request, "  <a href=\"%s/%d/events\">events</a>\n",
		prefix, child->id);
  }
  http_printf(request, "</PRE>\n");
  http_printf(request, "</BODY></HTML>");
  return (0);
}

/*
 * Expects URI of form:
 *     <prefix>   - All-children summary
 *     <prefix>/<id>  - Summary for child #id
 *     <prefix>/<id>/<detail>  - detail for child #id.
 */
int
nanny_children_http_status(struct http_request *request)
{
  struct nanny_child *child;
  char prefix[64];
  char *p;
  int id;

  p = request->uri;
  if (*p == '/')
    ++p;
  while (p[0] != '/' && (p[1] < '0' || p[1] > '9')) {
    if (p[0] == '\0')
      return nanny_children_http(request, request->uri);
    ++p;
  }
  if (p - request->uri > 60) /* Prefix must be shorter than 60 chars. */
    return nanny_children_http(request, "");
  memcpy(prefix, request->uri, p - request->uri);
  prefix[p - request->uri] = '\0';
  ++p; /* Skip the '/' */
  if (*p < '0' || *p > '9')
    return nanny_children_http(request, prefix);
  id = 0;
  while (*p >= '0' && *p <= '9')
    id = id * 10 + *p++ - '0';
  if (*p != '/' && *p != '\0')
    return nanny_children_http(request, prefix);
  child = live_children_oldest;
  while (child != NULL && child->id != id)
    child = child->younger;
  if (child == NULL)
    return nanny_children_http(request, prefix);
  /* We've identified a child. */
  if (*p == '\0')
    return nanny_children_http_child(request, child);
  /* User is asking for child detail. */
  ++p;
  if (strcmp(p, "stdout") == 0) {
    return nanny_children_http_child_log(request, child,
					 child->child_stdout, "STDOUT");
  } else if (strcmp(p, "stderr") == 0) {
    return nanny_children_http_child_log(request, child,
					 child->child_stderr, "STDERR");
  } else if (strcmp(p, "events") == 0) {
    return nanny_children_http_child_log(request, child,
					 child->child_events, "EVENTS");
  }
  /* Didn't recognize detail request, just give child summary. */
  return nanny_children_http_child(request, child);
}

/*
 *
 * UTILITIES
 *
 */

/*
 * sigchld signal handler.
 *
 * Using two separate counts---a count of signals received and a
 * separate count of signals processed---is race-free since each count
 * is updated in only one place.  (It only assumes that writing
 * integers to memory is atomic, which is generally a pretty safe
 * assumption.)
 */
static void sigchld_handler(int sig)
{
  /* If you do anything more complex than this, be sure to preserve errno! */
  nanny_globals.sigchld_count++;
}

/*
 * Start a background process.
 *
 * If the 'child' argument is non-NULL, then this sets up the stdout
 * and stderr collection.  If 'child' is NULL, this just forks and
 * runs the specified command.  In either case, it returns the PID of
 * the spawned process.
 */
static int run(int oldpid, const char **envp,
	       struct nanny_log *stdoutbuff,
	       struct nanny_log *stderrbuff,
	       const char *cmd)
{
  int i;
  int childpid;
  int stdout_pipe[2];
  int stderr_pipe[2];

  /* Is child already running? */
  if (oldpid != 0 && kill(oldpid, 0) == 0)
      return (oldpid);

  if (stdoutbuff != NULL) {
    if (0 != pipe(stdout_pipe)) {
      /* XXX TODO XXXX ERROR HANDLING XXXX */
      stdoutbuff = NULL;
    }
  }
  if (stderrbuff != NULL) {
    if (0 != pipe(stderr_pipe)) {
      /* XXX TODO XXXX ERROR HANDLING XXXX */
      stderrbuff = NULL;
    }
  }

  childpid = fork();
  if (childpid < 0) {
    perror("fork");
    exit(1);
  }
  if (childpid == 0) {
    /* The children are the future! */
    /* If we're collecting stdout, remap stdout to the pipe. */
    if (stdoutbuff != NULL) {
      close(stdout_pipe[0]);  /* Close read side */
      if (stdout_pipe[1] != 1) { /* Map write side to stdout */
	dup2(stdout_pipe[1], 1);
	close(stdout_pipe[1]);
      }
    }
    /* If we're collecting stderr, remap stdout to the pipe. */
    if (stderrbuff != NULL) {
      close(stderr_pipe[0]);
      if (stderr_pipe[1] != 2) {
	dup2(stderr_pipe[1], 2);
	close(stderr_pipe[1]);
      }
    }
    /* Close everything except stdio. */
    for (i = getdtablesize(); i > 2; --i)
      close(i);

    execle("/bin/sh", "/bin/sh", "-c", cmd, NULL, envp);

    /* If execle() returns, something is very, very wrong. */
    /* XXXX TODO: More interesting logging here. */
    perror("execle");
    exit(1);
  }

  if (stdoutbuff != NULL) {
    close(stdout_pipe[1]);
    nanny_log_from_fd(stdout_pipe[0], stdoutbuff);
  }
  if (stderrbuff != NULL) {
    close(stderr_pipe[1]);
    nanny_log_from_fd(stderr_pipe[0], stderrbuff);
  }
  return (childpid);
}

/*
 *
 * HEALTH CHECK PROCESS MANAGEMENT
 *
 */

/* Forward reference. */
static void main_child_goal_restart(void *_child, time_t now);

/*
 * Clean up when a health check completes.
 */
static void
health_check_ended(struct nanny_child *check, int stat, struct rusage *rusage)
{
  struct nanny_child *child = check->main;

  /* Release the health check. */
  child_free(check);

  /* If the health check succeeded, we're done. */
  if (WIFEXITED(stat) && WEXITSTATUS(stat) == 0) {
    child->health_failures_consecutive = 0;
    ++child->health_successes_consecutive;
    ++child->health_successes_total;
    return;
  }

  /* Report this failure. */
  if (WIFEXITED(stat)) {
    /* Health check finished with non-zero status. */
    nanny_log_printf(child->child_events,
		       "%s: Health check failed with exit code %d\n",
		       nanny_isotime(0), WEXITSTATUS(stat));
  } else if (WIFSIGNALED(stat)) {
    /* Health check failed with an error. */
    nanny_log_printf(child->child_events,
		     "%s: Health check exited on signal %d\n",
		     nanny_isotime(0), WTERMSIG(stat));
  } else {
    /* Yuck. */
  }

  /* Count failures and possibly act on them. */
  child->health_successes_consecutive = 0;
  child->health_failures_consecutive++;
  child->health_failures_total++;
  nanny_log_printf(child->child_events,
		     "%s: %d consecutive failures\n",
		     nanny_isotime(0),
		     child->health_failures_consecutive);
  if (child->health_failures_consecutive > 4) {
    /* Force a restart. */
    child->state_handler = main_child_goal_restart;
    nanny_timer_delete(child->state_timer);
    child->state_timer = nanny_timer_add(0, child->state_handler, child);
  }
}

/*
 * Supervise the health check.
 *
 * This is a very simple state manager that simply starts the
 * health check and kills it if it runs too long.
 */
static void
health_check_goal(void *_check, time_t now)
{
  struct nanny_child *check = _check;
  struct nanny_child *child = check->main;
  check->state_timer = NULL;

  /*
   * The health check is either "NEW" or "STARTING."  If it's "NEW",
   * we start it.  If "STARTING" times out, then we send SIGKILL.
   * The child-ending script will see the child exit and handle
   * the failure.
   */
  if (check->state == NEW) {
    check->pid = run(check->pid, check->envp,
		     check->child_stdout, check->child_stderr,
		     check->start_cmd);
    nanny_log_printf(child->child_events,
		       "%s: Started health check, pid=%d\n",
		       nanny_isotime(0), check->pid);
    check->ended = health_check_ended;
    check->running = 1;
    check->last_start = now;
    check->state = STARTING; /* Time out check after 60 seconds. */
    check->state_timer = nanny_timer_add(now + HEALTH_TIMEOUT,
				   check->state_handler, check);
    return;
  } else {
    nanny_log_printf(child->child_events,
		       "%s: Killing health check, pid=%d\n",
		       nanny_isotime(0), check->pid);
    kill(check->pid, SIGKILL); /* Just kill it. */
    return;
  }
  return;
}

/*
 *
 * MAIN CHILD MANAGEMENT
 *
 */

/*
 * Clean up when a main child exits.  This includes logging and
 * trying to restart the child, if appropriate.
 */
static void
main_child_ended(struct nanny_child *child, int stat, struct rusage *rusage)
{
  int pid = child->pid; /* Remember pid for reporting at end. */

  /* This child is no longer running. */
  child->pid = 0;
  if (child->id == 0)
    nanny_globals.child_pid = 0;
  child->ended = NULL;
  child->state = STOPPED;
  child->running = 0;
  child->last_stop = nanny_globals.now;
  /* Record the failure. */
  child->failures++;

  /* Exponential backoff:  min=1s, max=1h */
  child->restart_delay *= 2;
  if (child->restart_delay < 1)
    child->restart_delay = 1;
  if (child->restart_delay > 3600)
    child->restart_delay = 3600;

  /* Stop any pending timer. */
  nanny_timer_delete(child->state_timer);
  child->state_timer = NULL;
  nanny_timer_delete(child->health_timer);
  child->health_timer = NULL;

  /* Invoke the goal handler to process the state change. */
  nanny_timer_add(0, child->state_handler, child);

  /* Report the stopped child to the multicast group and to the event log. */
  if (WIFEXITED(stat)) {
    int status = WEXITSTATUS(stat);
    udp_announce("STOPPED\tID=%d\tPID=%d\tSTATUS=%d\tINSTANCE=%s\tCMD=%s",
		 child->id, pid, status,
		 child->instance != NULL ? child->instance : "",
		 child->start_cmd);
    nanny_log_printf(child->child_events,
		       "%s: STOPPED\tPID=%d\tSTATUS=%d\n",
		       nanny_isotime(0), pid, status);
  } else if (WIFSIGNALED(stat)) {
    int sig = WTERMSIG(stat);
    udp_announce("STOPPED\tID=%d\tPID=%d\tSIGNAL=%d\tINSTANCE=%s\tCMD=%s",
		 child->id, pid, sig,
		 child->instance != NULL ? child->instance : "",
		 child->start_cmd);
    nanny_log_printf(child->child_events,
		       "%s: STOPPED\tPID=%d\tSIGNAL=%d\n",
		       nanny_isotime(0), pid, sig);
  } else {
    /* TODO: Huh?  The child doesn't have an exit status and didn't
     * die from a signal.  I think the only other option is "stopped",
     * but I'm not sure there's any reasonable way to handle that. */
  }
  /* TODO: Report some basic rusage data to child->child_events, perhaps? */
}

static void
main_child_health_check(void *_child, time_t now)
{
  struct nanny_child *child = _child;
  struct nanny_child *check;

  /* Erase the fired timer. */
  child->health_timer = NULL;
  if (child->health_cmd == NULL) {
    /* Nonexistent health check always succeeds. */
    ++child->health_successes_total;
    ++child->health_successes_consecutive;
  } else {
    /* Spawn off a health check for this child. */
    check = child_alloc(child->health_cmd);
    nanny_child_set_envp(check, child->envp);
    check->state = NEW;
    check->state_handler = health_check_goal;
    check->state_timer = nanny_timer_add(0, check->state_handler, check);
    check->main = child;
    check->child_stderr = child->child_events;
    nanny_log_retain(check->child_stderr);
    check->child_stdout = child->child_events;
    nanny_log_retain(check->child_stdout);
    check->child_events = child->child_events;
    nanny_log_retain(check->child_events);
  }

  /* Reschedule the next health check. */
  child->health_timer = nanny_timer_add(now + HEALTH_PERIOD,
				  main_child_health_check, child);
}


/*
 * Try to make the child run.
 */
static void
main_child_goal_running(void *_child, time_t now)
{
  struct nanny_child *child = _child;

  /* Forget the timer that started us. */
  child->state_timer = NULL;

  /*
   * Trying to start the process.
   */

  /* Child failed: Decide whether to restart. */
  if (child->state == STOPPED) {
    if (child->restartable) {
      child->state = RESTARTING;
      nanny_timer_add(nanny_globals.now + child->restart_delay,
		child->state_handler, child);
      return;
    }
  }

  /* Child is ready to be started. */
  if (child->state == RESTARTING
      || child->state == NEW) {
    child->pid = run(child->pid, child->envp,
		     child->child_stdout, child->child_stderr,
		     child->start_cmd);
    if (child->id == 0)
      nanny_globals.child_pid = child->pid;
    /* Restart clears consecutive failures and consecutive successes. */
    child->health_failures_consecutive = 0;
    child->health_successes_consecutive = 0;
    child->ended = main_child_ended; /* How to handle termination. */
    child->running = 1;
    child->last_start = now;
    child->start_count++;

    udp_announce("%s\tPID=%d\tCMD=%s",
		 child->state == NEW ? "STARTING" : "RESTARTING",
		 child->pid,
		 child->start_cmd);
    nanny_log_printf(child->child_events,
		       "%s: %s\tPID=%d\tCMD=%s\n",
		       nanny_isotime(0),
		       child->state == NEW ? "STARTING" : "RESTARTING",
		       child->pid,
		       child->start_cmd);

    child->state = STARTING; /* Child is on probation. */
    child->state_timer = nanny_timer_add(now + HEALTH_PERIOD * 5,
				   child->state_handler, child);
    /* First health check is in 60 seconds. */
    child->health_timer = nanny_timer_add(now + HEALTH_PERIOD,
				    main_child_health_check, child);
    return;
  }

  if (child->state == STARTING) {
    /* If we've passed at least 5 health checks, bring us out of probation. */
    if (child->health_successes_consecutive > 4) {
      child->state = RUNNING;
      child->failures = 0;  /* A clean start. */
      child->restart_delay = 1;
      return;
    } else {
      child->state_timer = nanny_timer_add(now + HEALTH_PERIOD,
					   child->state_handler, child);
    }
  }

  /* XXX TODO XXX Deal with other states? XXX */
  return;
}

/*
 * Trying to stop the process.
 */
static void
main_child_goal_stopped(void *_child, time_t now)
{
  struct nanny_child *child = _child;
  child->state_timer = NULL;

  printf("main_child_goal_stopped\n");

  /* Not running or never started.  We're done. */
  if (child->pid == 0 || child->state == STOPPED
      || child->state == RESTARTING || child->state == NEW) {
    child->state = STOPPED;
    return;
  }

  /* PID doesn't exist:  Child exited and noone told us? */
  if (kill(child->pid, 0) == -1) {
    child->state = STOPPED;
    child->pid = 0;
    return;
  }

  /* The initial stop attempt is at the bottom. */

  /* Try SIGTERM. */
  if (child->state == STOPPING1) {
    child->state = STOPPING2;
    kill(child->pid, SIGTERM);
    nanny_log_printf(child->child_events,
		       "%s: SENDING SIGTERM to PID=%d\n",
		       nanny_isotime(0), child->pid);
    child->state_timer = nanny_timer_add(now + 15, child->state_handler, child);
    return;
  }

  /* We've tried playing nice:  send a SIGKILL. */
  if (child->state == STOPPING2) {
    child->state = STOPPING3;
    kill(child->pid, SIGKILL);
    nanny_log_printf(child->child_events,
		       "%s: SENDING SIGKILL to PID=%d\n",
		       nanny_isotime(0), child->pid);
    child->state_timer = nanny_timer_add(now + 15, child->state_handler, child);
    return;
  }

  /* Even SIGKILL didn't work?!  We give up. */
  if (child->state == STOPPING3) {
    udp_announce("UNSTOPPABLE\tPID=%d\tINSTANCE=%s\tCMD=%s",
		 child->pid, child->instance, child->start_cmd);
    /* TODO: Log this. */
    kill(child->pid, SIGKILL); /* Send one last shot. */
    nanny_log_printf(child->child_events,
		       "%s: SENDING SIGKILL to PID=%d\n",
		       nanny_isotime(0), child->pid);
    nanny_log_printf(child->child_events,
		       "%s: GIVING UP ON PID=%d\n",
		       nanny_isotime(0), child->pid);
    child->state = STOPPED;  /* Pretend it succeeded. */
    child->pid = 0;
    return;
  }

  /* Default case: We haven't yet asked the child to stop. */
  if (child->stop_cmd != NULL && child->stop_cmd[0] != '\0') {
    /* TODO: error handling. */
    /* give the stop command access to the nanny child PID */
    if (child->pid > 0) {
      char *buf;
      size_t len;
      for (len = 0; child->envp[len] != NULL; len++)
          ; /* do nothing */
      if ((buf = malloc(ENVLEN)) == NULL)
          _exit(1);
      memset(buf, '\0', ENVLEN);
      snprintf(buf, ENVLEN, "PID=%d", child->pid);
      child->envp[len] = buf;
    }
    run(0, child->envp, child->child_events, child->child_events,
	child->stop_cmd);
    /* Report that we're stopping it. */
    nanny_log_printf(child->child_events,
		       "%s: STOPPING\tPID=%d\tCMD=%s\n",
		       nanny_isotime(0), child->pid, child->stop_cmd);
    child->state = STOPPING1;
  } else {
    /* If there's no custom script, use SIGTERM. */
    child->state = STOPPING2;
    kill(child->pid, SIGTERM);
    /* Report that we're stopping it. */
    nanny_log_printf(child->child_events,
		       "%s: STOPPING\tPID=%d\tSIGNAL=%d\n",
		       nanny_isotime(0), child->pid, SIGTERM);
  }
  /* Give the child a generous amount of time to shutdown. */
  /* TODO: Make this probation period customizable. */
  /* Note: Other stop probations don't need to be customizable! */
  child->state_timer
    = nanny_timer_add(now + STOP_PROBATION, child->state_handler, child);
  return;
}

/*
 * Restart the child.
 */
static void
main_child_goal_restart(void *_child, time_t now)
{
  struct nanny_child *child = _child;

  if (child->state == STOPPED) {
    child->state = RESTARTING;
    child->state_handler = main_child_goal_running;
    child->state_timer = nanny_timer_add(0, child->state_handler, child);
  } else
    main_child_goal_stopped(_child, now);
}


/*
 * Record a new "main" child.  Set it up to start at the next opportunity.
 */
/*
int
nanny_new_child(const char *instance,
		const char *start_cmd,
		const char *stop_cmd,
		const char *health_cmd,
		const char **envp,
		int restartable)
*/
struct nanny_child *
nanny_child_new(const char *start_cmd)
{
  struct nanny_child *child;

  /* Install our sigchld handler. */
  signal(SIGCHLD, sigchld_handler);

  /* Register this child. */
  child = child_alloc(start_cmd);

  /* Set the state engine to start it at the next opportunity. */
  child->state = NEW;
  child->state_handler = main_child_goal_running;
  child->state_timer = nanny_timer_add(0, child->state_handler, child);
  child->child_stdout = nanny_log_alloc(65536);
  child->child_stderr = nanny_log_alloc(65536);
  child->child_events = nanny_log_alloc(65536);

  return (child);
}

void
nanny_child_set_logpath(struct nanny_child *child, const char *path)
{
  nanny_log_set_filename(child->child_stdout, "%s/nanny_stdout.log", path);
  nanny_log_set_filename(child->child_stderr, "%s/nanny_stderr.log", path);
  nanny_log_set_filename(child->child_events, "%s/nanny_event.log", path);
}

void
nanny_oversee_children(void)
{
  pid_t pid;
  int stat;
  struct rusage rusage;

  /* If we've handled all sigchld signals, then we've nothing to do here. */
  if (nanny_globals.sigchld_count == nanny_globals.sigchld_handled)
    return;
  nanny_globals.sigchld_handled++;

  /* Get information about a terminated child. */
  pid = wait3(&stat, WNOHANG, &rusage);
  while (pid > 0) {
    struct nanny_child *child = live_children_oldest, *next;
    while (child != NULL) {
      next = child->younger;
      if (child->pid == pid) {
	if (child->ended != NULL)
	  (child->ended)(child, stat, &rusage);
	break;
      }
      child = next;
    }
    pid = wait3(&stat, WNOHANG, &rusage);
  }
}

int
nanny_stop_all_children(void)
{
  struct nanny_child *child;
  struct timed_t *t;
  int alive = 0;

  for (child = live_children_oldest; child != NULL; child = child->younger) {
    if (child->state == STOPPED) { /* Skip child already stopped. */
      child->state_handler = main_child_goal_stopped; /* So it stays stopped. */
      continue;
    }
    ++alive; /* Count it as still alive. */
    if (child->state_handler == main_child_goal_stopped) /* Already stopping. */
      continue;
    /* Terminate periodic tasks. */
    while (child->timed != NULL) {
      /* Remove next periodic task */
      t = child->timed;
      child->timed = child->timed->next;
      /* Free it */
      nanny_timer_delete(t->timer);
      free(t->cmd);
      free(t);
    }
    child->timed = NULL;

    /* Shutdown pending timers. */
    nanny_timer_delete(child->state_timer);
    child->state_timer = NULL;
    nanny_timer_delete(child->health_timer);
    child->health_timer = NULL;
    /* Switch this child to the "stopping" state machine. */
    child->state_handler = main_child_goal_stopped;
    child->state_timer = nanny_timer_add(0, child->state_handler, child);
  }
  return (alive);
}

int
nanny_timed_http_status(struct http_request *request, struct nanny_child *child)
{
  struct timed_t *t;
  const char *sep = "\n";

  http_printf(request, "HTTP/1.0 200 OK\x0d\x0a");
  http_printf(request, "Content-Type: text/plain\x0d\x0a");
  http_printf(request, "\x0d\x0a");
  http_printf(request, "[");
  for (t = child->timed; t != NULL; t = t->next) {
    http_printf(request, "%s", sep);
    http_printf(request, "  {\n");
    http_printf(request, "    \"cmd\": \"%s\",\n", t->cmd);
    http_printf(request, "    \"interval\": %d,\n", t->interval);
    if (t->last)
      http_printf(request, "    \"last\": \"%s\",\n", nanny_isotime(t->last));
    http_printf(request, "    \"next\": \"%s\"\n", nanny_isotime(nanny_timer_expiration(t->timer)));
    http_printf(request, "  }");
    sep = ",\n";
  }
  http_printf(request, "\n]\n");
  return (0);
}

/*
 * Fire off a timed event.
 *
 * TODO: harvest stdout/stderr back to the main nanny for logging
 * to the event log.
 *
 * TODO: support configurable target email and allow multiple email addresses.
 *
 * Question:  Is the event log a suitable substitute for emailing
 * the output?  The email approach mimics cron; there may be something
 * better.
 */
static void
timed_event(void *t0, time_t now)
{
  char buff[4096];
  const char *p, *username;
  struct timed_t *t = (struct timed_t *)t0;
  int childpid;
  int task_pipe[2], sendmail_pipe[2];
  int i;
  ssize_t bytesread;

  t->last = now;

  /* Reschedule. */
  t->timer = nanny_timer_add(now + t->interval, timed_event, t0);

  /* Fork to handle the event in the background. */
  childpid = fork();
  if (childpid < 0) /* Fork failed; XXXX do something here XXXX */
    return;
  if (childpid != 0) /* Parent returns. */
    return;

  /* Move to a safe directory. */
  chdir("/tmp");
  username = nanny_username(); /* Look up username. */

  /* Clean up I/O descriptors. */
  for (i = getdtablesize(); i >= 0; --i) /* Close everything. */
      close(i);
  /* Redirect stdio to /dev/null. */
  open("/dev/null", O_RDONLY); /* stdin */
  open("/dev/null", O_WRONLY); /* stdout */
  open("/dev/null", O_WRONLY); /* stderr */

  /* Create a pipe to collect output from child. */
  pipe(task_pipe);

  childpid = fork();
  if (childpid < 0)
    exit(1);
  if (childpid == 0) {
    char *buf;
    /* Set "PID" environment variable to the PID of the main child. */
    if (nanny_globals.child_pid > 0) {
      if ((buf = malloc(ENVLEN)) == NULL)
          _exit(1);
      memset(buf, '\0', ENVLEN);
      snprintf(buf, ENVLEN, "PID=%d", nanny_globals.child_pid);
      t->envp[t->envplen] = buf;
      t->envplen++;
    }
    /* Set "NANNY_SCHEDULED" var to the time for which we were scheduled. */
    if ((buf = malloc(ENVLEN)) == NULL)
        _exit(1);
    memset(buf, '\0', ENVLEN);
    snprintf(buf, ENVLEN, "NANNY_SCHEDULED=%ld", (long)now);
    t->envp[t->envplen] = buf;
    t->envplen++;
    /* Exec /bin/sh to run child with stdout/stderr writing to pipe. */
    close(task_pipe[0]); /* Close read side in child. */
    dup2(task_pipe[1], 1); /* Redirect stdout/stderr to pipe. */
    dup2(task_pipe[1], 2);
    close(task_pipe[1]); /* Clean up extra pipe fd. */
    /* TODO: Make sure we're setting up the environment correctly. */
    execle("/bin/sh", "/bin/sh", "-c", t->cmd, NULL, t->envp);
    exit(1);
  } else {
    /* Read pipe in parent; if there's any output, send it to mail. */
    close(task_pipe[1]); /* Close the write side in parent. */

    /* Try to read from child. */
    for (;;) {
      bytesread = read(task_pipe[0], buff, sizeof(buff));
      if (bytesread == 0) /* End of file, so no output. */
	exit(0);
      /* Keep trying if there's an error or if we can't email. */
      if (bytesread < 0 || username == NULL)
	continue;
      break;
    }

    /* We have input from child; fire up a pipe to /usr/sbin/sendmail. */
    pipe(sendmail_pipe);
    childpid = fork();
    if (childpid < 0)
      exit(1);
    if (childpid == 0) {
      /* Run sendmail.... */
      close(sendmail_pipe[1]); /* Close write side in child. */
      dup2(sendmail_pipe[0], 0); /* Plug read side into stdin. */
      close(sendmail_pipe[0]);
      execlp("/usr/sbin/sendmail", "/usr/sbin/sendmail", username, NULL);
      exit(1);
    } else {
      close(sendmail_pipe[0]);
      /* TODO: XXX check for and abort on errors writing to sendmail */
      write(sendmail_pipe[1], "Subject: <", 10);
      /* Note: We know username is non-NULL here. */
      write(sendmail_pipe[1], username, strlen(username));
      write(sendmail_pipe[1], "@", 1);
      p = nanny_hostname();
      write(sendmail_pipe[1], p, strlen(p));
      write(sendmail_pipe[1], "> ", 2);
      write(sendmail_pipe[1], t->cmd, strlen(t->cmd));
      write(sendmail_pipe[1], "\n", 1);
      write(sendmail_pipe[1], "\n", 1);
      write(sendmail_pipe[1], "\n", 1);
      write(sendmail_pipe[1], buff, bytesread);
      for (;;) {
	bytesread = read(task_pipe[0], buff, sizeof(buff));
	if (bytesread < 0)
	  continue;
	if (bytesread == 0)
	  exit(0);
	write(sendmail_pipe[1], buff, bytesread);
      }
      exit(0);
    }
  }
}

static time_t
parse_interval(const char **p)
{
  const char *original = *p;
  time_t interval = 0;
  long i;

  for (;;) {
    if (**p < '0' || **p > '9') {
      fprintf(stderr,
	      "Invalid time specification (expecting number): %s\n",
	      original);
      return (-1);
    }
    i = 0;
    while (**p >= '0' && **p <= '9') {
      i *= 10;
      i += **p - '0';
      ++(*p);
    }
    switch (**p) {
    case 'd': interval += i * 86400; break;
    case 'h': interval += i * 3600; break;
    case 'm': interval += i * 60; break;
    case 's': interval += i; break;
    default:
      fprintf(stderr,
	      "Invalid time specification:%s\n"
	      "  Expected time unit 'd', 'h', 'm', or 's'\n",
	      original);
      return (-1);
    }
    ++(*p);
    if (**p == ' ' || **p == '\t') {
      while (**p == ' ' || **p == '\t')
	++(*p);
      return interval;
    }
  }
}

int
nanny_child_add_periodic(struct nanny_child *child, const char *task)
{
  const char *p;
  struct timed_t *t;
  int first_delay;
  int i, len;

  t = (struct timed_t *)malloc(sizeof(*t));
  memset(t, 0, sizeof(*t));

  p = task;
  t->interval = parse_interval(&p);
  if (t->interval < 0) {
    free(t);
    return (1);
  }
  if (*p == '\0') {
    fprintf(stderr, "No command specified for timed operation: %s\n", task);
    free(t);
    return (1);
  }
  t->cmd = strdup(p);
  if (t->cmd == NULL) {
    free(t);
    return (1);
  }
  if (child->envp != NULL) {
    /* take the child environment array, and create a copy which is a
     * little large, so we have space for a couple of special, timed-only
     * variables
     */
    for (len = 0; child->envp[len] != NULL; len++)
        ; /* do nothing */
    if ((t->envp = calloc(len + 3, sizeof(*t->envp))) == NULL) {
      fprintf(stderr, "nanny_child_add_periodic: calloc failure\n");
      exit(1);
    }
    for (i = 0; i < len; i++) {
      t->envp[i] = child->envp[i];
    }
  }
  t->envplen = i;

  t->next = child->timed;
  child->timed = t;

  srandom(getpid());
  first_delay = random() % t->interval;
  t->timer = nanny_timer_add(time(NULL) + first_delay, timed_event, t);

  return (0);
}
