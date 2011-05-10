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

#include <sys/types.h> /* struct sockaddr_in */
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

/* Push the current process into the background; save PID to pidfile. */
void nanny_daemonize(const char *pidfile);

/*
 * Nanny's global values.
 */
extern struct nanny_globals_t {
  time_t now;  /* Result of last call to time(). */
  int http_port; /* Port opened by http server creation. */
  int udp_unicast_socket; /* udp_announce sends messages using this socket */
  struct sockaddr_in udp_multicast_addr; /* udp_announce sends messages here */
  int sigchld_count;
  int sigchld_handled;
  int nanny_pid;
  int child_pid;
} nanny_globals;

/* Return the value of a variable. */
/* This recognizes a handful of well-known keys, including HTTP_PORT, PID, and UID. */
/* It will then look in the shell environment and finally in XXXX TODO XXXX */
const char *nanny_variable(const char *);
int nanny_variable_compare(const char *key, const char *value);

/* Basic information about the multicast socket. */
#define MULTICAST_ADDR	"226.1.1.1"
#define MULTICAST_PORT	8889

/*
 * Core routines to register and poll select-driven servers.
 */
void nanny_register_server(void (*handler)(void *), int s, void *data);
void nanny_unregister_server(int fd);
void nanny_select(struct timeval *);

/*
 * HTTP server support.
 */

/* A "request" contains information about a single HTTP transaction. */
struct http_request {
  struct http_connection *connection;
  char *uri;
  int method;
  const char *method_name;
  int HTTPmajor;
  int HTTPminor;

  /* Called for each header in the HTTP request.  If not set,
   * headers are discarded. */
  int (*header_processor)(struct http_request *,
			  const char *key, const char *value);
  /* Called after all headers are read. */
  int (*body_processor)(struct http_request *);

  void *data;
};

#define HTTP_METHOD_GET 1
#define HTTP_METHOD_PUT 2
#define HTTP_METHOD_POST 3

/* Initialize the HTTP server. */
/* The 'dispatcher' is handed the request and given an opportunity
 * to fill in header_processor and body_processor functions. */
void http_server_init(const struct sockaddr *,
		      socklen_t namelen,
		      void (*dispatcher)(struct http_request *));

/* Write response data back. */
void http_printf(struct http_request *, const char *fmt, ...);
ssize_t http_write(struct http_request *, void *, size_t);

/* Generate a dump of the current environment. */
int nanny_http_environ_body(struct http_request *);

/*
 * UDP server.
 */

/*
 * Initialize a UDP server:
 *   If port != -1, bind to the specified port.
 *   If mcaddress != NULL, register with the multicast address.
 */
void udp_server_init(const char *mcaddress, int port);
/* Send a message to the multicast group. */
void udp_announce(char *fmt, ...);

/*
 * There are some assumptions in the UDP server code that
 * you will create two UDP servers:  a multicast server and
 * a unicast server.  udp_announce by default uses the unicast
 * port to send messages and uses the address/port used to
 * create a multicast server as the destination for the message.
 */

/*
 * Logging
 */
struct nanny_log;
struct nanny_log *nanny_log_alloc(size_t);
void nanny_log_set_filename(struct nanny_log *, const char *fmt, ...);
void nanny_log_retain(struct nanny_log *);
void nanny_log_release(struct nanny_log *);
void nanny_log_printf(struct nanny_log *, char *, ...);
void nanny_log_from_fd(int fd, struct nanny_log *);
void nanny_log_http_dump_raw(struct http_request *, struct nanny_log *);
void nanny_log_http_dump_json(struct http_request *, struct nanny_log *,
			      const char * /*name*/, const char * /*indent*/);

/*
 * Child process management.
 */

/*
 * Information about a child process.  This structure handles both
 * "main" child processes and some auxiliary processes (such as health
 * checks, which we need to time out).  Other auxiliary processes
 * (such as stop scripts) just get started and abandoned and so don't
 * get an entry here.  Maybe that should change....
 */
struct nanny_child {
  /* Live list management. */
  struct nanny_child *older; /* Next older sibling */
  struct nanny_child *younger; /* Next younger sibling */

  /* Information provided by the client. */
  char *instance;
  char *start_cmd;
  char *stop_cmd;
  char *health_cmd;
  int restartable;

  /* Basic status. */
  int id; /* Our internal ID which doesn't vary, even if PID does. */
  int pid;  /* PID of current process; 0 if none. */
  int running;  /* True if child is believed to be running. */
  time_t last_start;
  time_t last_stop;
  int start_count;
  int failures;  /* Consecutive failures. */
  int restart_delay; /* In seconds. */

  /* How to handle this child when it stops. */
  void (*ended)(struct nanny_child *, int stat, struct rusage *);

  /* Timer and handler function for driving state transitions. */
  /* This drives state machines for startup, shutdown, etc. */
  void (*state_handler)(void *_child, time_t now);
  struct timer *state_timer; /* Pending timer for this child, if any. */
  const char *state; /* What state is the child in? */

  /* Periodic events for this child. */
  struct timed_t *timed;

  /* Health check information. */
  struct nanny_child *main; /* If this is health check, child being checked. */
  struct timer *health_timer;  /* Timer for starting next health check. */
  /* Counts of health checks that failed or succeeded. */
  int health_failures_consecutive;
  int health_failures_total;
  int health_successes_consecutive;
  int health_successes_total;

  /* Buffers for stdout, stderr, and event logging. */
  /* stdout/stderr from health checks also write to the events log. */
  struct nanny_log *child_stderr;
  struct nanny_log *child_stdout;
  struct nanny_log *child_events;

  /* Environment array for execve() */
  const char  **envp;
};

/*
 * Create a new child object, set properties of the child.
 */
struct nanny_child *nanny_child_new(const char *start);
/* Set shell command to run at stop. */
void nanny_child_set_stop(struct nanny_child *, const char *);
/* Set path to log directory. */
void nanny_child_set_logpath(struct nanny_child *, const char *);
/* Set command to run for regular health checks. */
void nanny_child_set_health(struct nanny_child *, const char *);
/* Set true to automatically restart child. */
void nanny_child_set_restartable(struct nanny_child *, int);
/* Environment to pass down to processes. */
void nanny_child_set_envp(struct nanny_child *, const char **);
/* Add a periodic task to this child. */
int nanny_child_add_periodic(struct nanny_child *, const char *);

/*
 * Register a new child all at once...
 *   id: a token printed out to identify this child
 *   start:  shell command line to start the child
 *   restart:  shell command line to restart the child.  If NULL, then
 *      child will not be restarted.
 *   stop:  shell command line to stop the child.  Note that the
 *      stop logic sets the environment value PID to the PID of the child,
 *      in order to simplify the common usage "kill -QUIT ${PID}".
 *   status: shell command line to probe child and generate textual status to
 *      stdout.  Note that PID is set as for 'stop.'
 *   envp: environment array passed to execve().
 */
int nanny_new_child(const char *id,
		    const char *start,
		    const char *stop,
		    const char *status,
		    const char **envp,
		    int restartable);
/* Poll the children to see if there are any state changes to handle. */
/* In particular, call this if any system call is interrupted, since it
 * might be a SIGCHLD. */
void nanny_oversee_children(void);
/* Returns 'true' when all children are stopped. */
int nanny_stop_all_children(void);
/* Generate an HTTP page with child status information. */
int nanny_children_http_status(struct http_request *request);

/*
 * Useful utility functions.
 */
const char *nanny_hostname(void);
const char *nanny_username(void);
const char *nanny_isotime(time_t);

/*
 * Counter server.
 */
void *nanny_counter_server_init(const char *pathname);
void nanny_counter_server_close(void *);

size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
