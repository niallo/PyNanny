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
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "nanny.h"

/*
 * A very basic forking web server.
 *
 * The forking design here is a very deliberate design choice that
 * emphasizes simplicity and robustness over performance.  (Although
 * the performance may be better than you expect!)
 */

/*
 * The server has three layers:  a listening "server," client "connections,"
 * and "requests" on those connections.  The server forks for each connection.
 */

/* A server is just a listening TCP socket and a connection dispatcher. */
struct http_server {
  int sock;
  int port;
  void (*dispatcher)(struct http_request *);
};

/*
 * A "connection" is a socket with some I/O buffering machinery and a
 * small amount of other connection-level state.
 */
struct http_connection {
  int sock;
  int keepalive;

  /* The input buffer. */
  char *buff;
  char *buff_end;
  size_t buff_size;
  /* The start and end of live data within the input buffer. */
  char *start;
  char *end;
};

/*
 * Custom character classification bitmap:
 *   0x10 = character allowed in URI
 *   0x20 = character allowed as first char of path component (alphanumerics)
 *   0x40 = character allowed in path component (alphanumerics and -_.)
 *   Lower nybble is used for char->hex digit conversion.
 */
static const char uri_map[] = {
  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00, /* 00-0F */
  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00, /* 10-1F */
  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
  0x00, 0x10, 0x00, 0x00,  0x10, 0x00, 0x10, 0x10, /* 20-2F */
  0x10, 0x10, 0x00, 0x10,  0x10, 0x50, 0x50, 0x10,
  0x70, 0x71, 0x72, 0x73,  0x74, 0x75, 0x76, 0x77, /* 30-3F */
  0x78, 0x79, 0x10, 0x10,  0x00, 0x10, 0x00, 0x10,
  0x10, 0x7a, 0x7b, 0x7c,  0x7d, 0x7e, 0x7f, 0x70, /* 40-4F */
  0x70, 0x70, 0x70, 0x70,  0x70, 0x70, 0x70, 0x70,
  0x70, 0x70, 0x70, 0x70,  0x70, 0x70, 0x70, 0x70, /* 50-5F */
  0x70, 0x70, 0x70, 0x00,  0x00, 0x00, 0x00, 0x50,
  0x10, 0x7a, 0x7b, 0x7c,  0x7d, 0x7e, 0x7f, 0x70, /* 60-6F */
  0x70, 0x70, 0x70, 0x70,  0x70, 0x70, 0x70, 0x70,
  0x70, 0x70, 0x70, 0x70,  0x70, 0x70, 0x70, 0x70, /* 70-7F */
  0x70, 0x70, 0x70, 0x00,  0x00, 0x00, 0x10, 0x00,
};

/* Is character allowed in a URI? */
#define uri_okay(c)	(c > 0 && c < 127 && (uri_map[(int)c] & 0x10))

ssize_t
http_write(struct http_request *request, void *buff, size_t s)
{
  return write(request->connection->sock, buff, s);
}

void
http_printf(struct http_request *request, const char *fmt, ...)
{
  char msg[8192];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  http_write(request, msg, strlen(msg));
}

/* Standard 404 handler is invoked unless dispatcher overrides. */
static int
body404(struct http_request *request)
{
  http_printf(request, "HTTP/1.0 404 NOT FOUND\x0d\x0a");
  http_printf(request, "Content-Type: text/plain\x0d\x0a");
  http_printf(request, "\x0d\x0a");
  http_printf(request, "URI not found: %s\n", request->uri);
  return (0);
}

/*
 * Read a single line from the HTTP connection.
 * TODO: Try to avoid unnecessary data movement.
 */
static int
http_connection_readline(struct http_connection *connection, char **start, char **end)
{
  char *p;

  if (connection->end == connection->start) {
    connection->end = connection->start = connection->buff;
  } else if (connection->start > connection->buff) {
    memmove(connection->buff, connection->start, connection->end - connection->start);
    connection->end = connection->buff + (connection->end - connection->start);
    connection->start = connection->buff;
  }

  p = *end = *start = connection->start;
  while (*p != '\n') {
    ++p;
    if (p >= connection->end) {
      ssize_t bytes = read(connection->sock, connection->end,
		   connection->buff_end - connection->end);
      if (bytes <= 0)
	return -1;
      connection->end += bytes;
    }
  }
  connection->start = p + 1; /* Next input starts just after '\n' */
  *end = p;
  while (*end > *start && ((*end)[-1] == '\n' || (*end)[-1] == '\r'))
    --(*end);
  return (*end - *start);
}

static int
http_parse_version(struct http_request *request, char **pp)
{
  char *p = *pp;

  /* Check the format of the HTTP-version string. */
  if (memcmp(p, "HTTP/", 5) != 0)
    return (1);
  p += 5;
  /* Major version is one or two decimal digits. */
  if (*p < '0' || *p > '9') {
    fprintf(stderr, "Bad major version number\n");
    return (1);
  }
  request->HTTPmajor = *p - '0';
  p++;
  if (*p >= '0' && *p <= '9') {
    request->HTTPmajor = request->HTTPmajor * 10 + (*p - '0');
    p++;
  }
  /* Mandatory decimal. */
  if (*p != '.') {
    fprintf(stderr, "Missing period in HTTP version\n");
    return (1);
  }
  p++;
  /* Minor version is one or two decimal digits. */
  if (*p < '0' || *p > '9') {
    fprintf(stderr, "Bad minor version number\n");
    return (1);
  }
  request->HTTPminor = *p - '0';
  p++;
  if (*p >= '0' && *p <= '9') {
    request->HTTPminor = request->HTTPminor * 10 + (*p - '0');
    p++;
  }
  *pp = p;
  return (0);
}

/*
 * Read and parse the initial request line.
 */
static int
http_request(struct http_request *request)
{
  char *p, *end, *start;

  http_connection_readline(request->connection, &p, &end);

  /* Method is one of a small set of possible strings. */
  switch (p[0]) {
  case 'G':
    if (memcmp(p, "GET ", 4) == 0) {
      request->method = HTTP_METHOD_GET;
      request->method_name = "GET";
      p += 4;
    }
    break;
  case 'P':
    if (memcmp(p, "PUT ", 4) == 0) {
      request->method = HTTP_METHOD_PUT;
      request->method_name = "PUT";
      p += 4;
    } else if (memcmp(p, "POST ", 5) == 0) {
      request->method = HTTP_METHOD_POST;
      request->method_name = "POST";
      p += 5;
    }
    break;
  default:
    fprintf(stderr, "Unsupported method\n");
    return (1);
  }
  /* URI extends to the next space. */
  /* Here, we just identify the string; we don't parse the URI. */
  start = p;
  while (*p > 0 && *p < 127 && uri_okay(*p) == 1)
    p++;
  if (p <= start) {
    request->uri = NULL;
    fprintf(stderr, "URI missing\n");
    return (1);
  }
  request->uri = malloc(p - start + 1);
  memcpy(request->uri, start, p - start);
  request->uri[p - start] = '\0';
  /* HTTP/m.n is technically optional. */
  if (*p == ' ') {
    p++;
    if (http_parse_version(request, &p))
      return (1);
  }
  /* Request line must end with CRLF */
  if (p[0] != '\x0d' || p[1] != '\x0a') {
    fprintf(stderr, "Improperly terminated request line.\n");
    return (1);
  }
  return (0);
}

static int
http_request_header(struct http_request *request)
{
  char *p, *end, *header;

  http_connection_readline(request->connection, &p, &end);
  if (p < end)
    *end = '\0';
  else {
    /* Blank line denotes end of headers. */
    return (0);
  }

  header = p;
  while (p < end && *p != ':') {
    *p = toupper(*p);
    ++p;
  }
  if (p >= end) {
    printf("No colon, bad header line.");
    return (0);
  }
  *p = '\0';
  ++p;

  while (p < end && (*p == ' ' || *p == '\t'))
    ++p;

  if (request->header_processor)
    (request->header_processor)(request, header, p);
  return (1);
}

/*
 * Read a request header, invoke the appropriate request
 * handler and loop until socket is closed.
 */
static void
http_connection(struct http_server *server, int sock)
{
  /* Stack allocation works for small structures. */
  struct http_connection _connection;
  struct http_connection *connection = &_connection;
  struct http_request _request;
  struct http_request *request = &_request;

  /* Initialize the connection I/O buffers. */
  memset(connection, 0, sizeof(*connection));
  connection->buff_size = 16384;
  connection->buff = malloc(connection->buff_size);
  connection->buff_end = connection->buff + connection->buff_size;
  connection->start = connection->end = connection->buff;
  connection->sock = sock;
  connection->keepalive = 0;

  /* Handle one or more requests over this connection. */
  do {
    memset(request, 0, sizeof(*request));
    request->connection = connection;

    if (http_request(request)) {
      /* TODO: Generate an HTTP response for this. */
      fprintf(stderr, "invalid request line\n");
      return;
    }

    /* Dispatch the request. */
    server->dispatcher(request);

    /* Read the headers. */
    while (http_request_header(request))
      ;

    if (request->body_processor)
      (request->body_processor)(request);
    else
      body404(request);

    /* If keepalive is enabled, get the next request. */
  } while (connection->keepalive);

  /* Return.  Child will exit and connection will implicitly be closed. */
}

static void
http_server_accept(void *_server)
{
  struct http_server *server = _server;
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  int s, r;

  s = accept(server->sock, (struct sockaddr *)&addr, &len);
  r = fork();
  switch (r) {
  case 0:
    /* Child process. */
    close(server->sock);
    /* TODO: Close any fd's we don't need. */
    http_connection(server, s);
    _exit(0);
  case -1:
    /* TODO: Log this. */
    perror("fork");
    return;
  default:
    close(s);
    return;
  }
}

/*
 * Register with the central dispatcher.
 */
void http_server_init(const struct sockaddr *name,
		      socklen_t namelen,
		      void (*dispatcher)(struct http_request *))
{
  struct http_server *server;
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  if ((server = malloc(sizeof(*server))) == NULL)
    perror("malloc");
  memset(server, 0, sizeof(*server));
  server->dispatcher = dispatcher;

  server->sock = socket(AF_INET, SOCK_STREAM, 0);
  /* if name is NULL, we listen on a random (assigned by kernel) address. */
  if (name != NULL) {
    if (bind(server->sock, name, namelen) == -1)
      perror("bind");
  }
  if (listen(server->sock, 128))
    perror("listen");

  if (getsockname(server->sock, (struct sockaddr *)&addr, &len))
    perror("getsockname");
  server->port = ntohs(addr.sin_port);
  nanny_globals.http_port = server->port;

  nanny_register_server(http_server_accept, server->sock, server);
}


/* A pre-packaged responder that clients can dispatch to. */
static void
nanny_http_json_string(struct http_request *request, const char *s, const char *sep)
{
  http_printf(request, "\"");
  while (s != NULL && *s != '\0') {
    if (sep != NULL && *s == sep[0]) {
      http_printf(request, "\": \"");
      sep = NULL;
    } else if (*s == '"' || *s == '\\') {
      http_printf(request, "\\%c", *s);
    } else if (*s < 32) {
      switch (*s) {
      case '\b': http_printf(request, "\\b"); break;
      case '\f': http_printf(request, "\\f"); break;
      case '\n': http_printf(request, "\\n"); break;
      case '\r': http_printf(request, "\\r"); break;
      case '\t': http_printf(request, "\\t"); break;
      default: http_printf(request, "\\u%04x", *s);
      }
    } else
      http_printf(request, "%c", *s);
    ++s;
  }
  http_printf(request, "\"");
}

int
nanny_http_environ_body(struct http_request *request)
{
  extern char **environ;
  static char *default_keys[] = {
    "GID", "HOSTNAME", "HTTP_PORT", "ISOTIME", "NANNY_PID",
    "PID", "TIME", "UID", "USERNAME", NULL };
  char **p;
  char *last;
  char *next;
  char *sep;

  http_printf(request, "HTTP/1.0 200 OK\x0d\x0a");
  http_printf(request, "Content-Type: text/plain\x0d\x0a");
  http_printf(request, "\x0d\x0a");
  http_printf(request, "{\n");
  sep = " ";
  for (p = default_keys; *p != NULL; p++) {
    http_printf(request, sep);
    nanny_http_json_string(request, *p, NULL);
    http_printf(request, ": ");
    nanny_http_json_string(request, nanny_variable(*p), NULL);
    sep = ",\n ";
  }
  sep = ",\n\n ";

  /* Emit the shell vars in sorted order.  Crude but it works. */
  for (p = environ, next = NULL; *p != NULL; p++) {
    if (next == NULL || strcmp(*p, next) < 0)
      next = *p;
  }
  while (next != NULL) {
    http_printf(request, sep);
    nanny_http_json_string(request, next, "=");
    last = next;
    for (p = environ, next = NULL; *p != NULL; p++)
      if (strcmp(*p, last) > 0)
	if (next == NULL || strcmp(*p, next) < 0)
	  next = *p;
    sep = ",\n ";
  }
  http_printf(request, "\n}\n");
  return (0);
}
