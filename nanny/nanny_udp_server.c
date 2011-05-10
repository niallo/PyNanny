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
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "nanny.h"

void
udp_announce(char *fmt, ...)
{
  char msg[8192];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  sendto(nanny_globals.udp_unicast_socket, msg, strlen(msg), 0,
	 (struct sockaddr *)&nanny_globals.udp_multicast_addr,
	 sizeof(nanny_globals.udp_multicast_addr));
}

struct udp_server {
  int sock;
  struct sockaddr_in addr;
  socklen_t addr_len;
};

static const char *
udp_query_match(const char *key, char op, const char *val)
{
  const char *myval;
  int cmp;

  myval = nanny_variable(key);
  if (myval == NULL)
    return NULL;
  cmp = nanny_variable_compare(key, val);

  switch (op) {
  case '=': if (cmp == 0) return myval; break;
  case '>': if (cmp < 0) return myval; break;
  case '<': if (cmp > 0) return myval; break;
  }
  /* Match failed. */
  return NULL;
}

/*
 * A simply query-by-example interface.
 * Requests are whitespace-separated lists of bare keys
 * and value assertions.  If all of the assertions are true,
 * a response is generated providing values for all keys.
 *
 * For example:
 *   Query: POD=p01 PROJECT=gd PORT<8200 INSTANCE
 * Might elicit a response of the form:
 *          POD=p01 PROJECT=gd PORT=8100 INSTANCE=gd#1
 *
 * Yes, there's an implicit 'AND' between the assertions.
 * Note that the response is always "KEY=VALUE" regardless of
 * how the key was specified in the query.
 */
static int
udp_query(char *p, char *dest, size_t destlen)
{
  char *destend = dest + destlen;
  const char *key, *value, *myval;
  char *keyend;
  char keysep, valuesep;
  char *outsep = "";

  *dest = '\0';

  while(*p != '\0') {
    /* Skip any whitespace. */
    while (*p == ' ' || *p == '\t')
      ++p;
    /* Identify beginning and end of key. */
    key = p;
    while (*p != '\0' && *p != ' ' && *p != '\t'
	   && *p != '=' && *p != '<' && *p != '>')
      ++p;
    if (p <= key)
      return 1;
    /* Null-terminate the key and remember the separating char. */
    keyend = p;
    keysep = *p;
    *p = '\0';

    /* Figure out what to do based on the character following the key. */
    switch (keysep) {
    case '=': case '<': case '>':
      /* Requester is asking for a match. */
      /* Identify the value to be matched. */
      value = ++p;
      while (*p != '\0' && *p != ' ' && *p != '\t')
	++p;
      /* If values don't match, return 0 */
      valuesep = *p;
      *p = '\0';
      myval = udp_query_match(key, keysep, value);
      if (myval == NULL)
	return 0;
      *p = valuesep; /* Restore the char following value. */
      break;
    case ' ': case '\t': case '\0':
      /* Look up value for this key; if we don't have it, return 0 */
      myval = nanny_variable(key);
      if (myval == NULL)
	return 0;
      break;
    default:
      return 0;
    }

    /* Append key=value to result. */
    if (dest + strlen(outsep) + strlen(key) + 1 + strlen(myval) >= destend)
      return 0;
    strlcat(dest, outsep, destlen);
    strlcat(dest, key, destlen);
    strlcat(dest, "=", destlen);
    strlcat(dest, myval, destlen);
    dest += strlen(dest);

    /* If key was at end of request, we're done. */
    if (keysep == '\0')
      break;
    *keyend = keysep;
    outsep = " ";
  }
  *dest = '\0';
  return 1;
}

static void
udp_query_response(char *buff, int sock,
		 struct sockaddr *addr0, socklen_t addrlen)
{
  char outbuff[2048];
  ssize_t sent;

  switch(buff[0]) {
  case '?':
    if (udp_query(buff + 1, outbuff, sizeof(outbuff))) {
      sent = sendto(sock, outbuff, strlen(outbuff), 0, addr0, addrlen);
      if (sent < 0)
	perror("sendto");
    }
    break;
  default:

#if 0
    { /* Debug message disabled by default. */
      char nows[32];
      struct sockaddr_in *addr = (struct sockaddr_in *)addr0;
      strftime(nows, sizeof(nows), "%H:%M:%S", localtime(&nanny_globals.now));
      printf("%s: Message from (%s:%d): %s\n",
	     nows, inet_ntoa(addr->sin_addr), addr->sin_port, buff);
    }
#endif
    break;
  }
}


static struct udp_server *
udp_server(const char *multicast_addr, int port)
{
  struct udp_server *server;

  struct sockaddr_in localaddr;
  struct ip_mreq mcChannel;
  const static int one = 1;

  server = malloc(sizeof(struct udp_server));
  memset(server, 0, sizeof(*server));

  server->sock = socket(AF_INET, SOCK_DGRAM, 0);

  /* Set up a specific port if one was specified. */
  if (port > 0) {
    /* Allow multiple copies of this code to share the same addr/port. */
    if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
      perror("multicast:setsockopt(SO_REUSEADDR)");
      exit(1);
    }

#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__)
    /* This seems to be required for proper operation of a multicast
     * UDP server on Mac OS, but not on Linux.  */
    if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one))) {
      perror("multicast:setsockopt(SO_REUSEPORT)");
      exit(1);
    }
#endif

    /* Listen on a well-known port. */
    memset(&localaddr, 0, sizeof(localaddr));
    localaddr.sin_family = AF_INET;
    localaddr.sin_port = htons(port);
    localaddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server->sock, (struct sockaddr *)&localaddr, sizeof(localaddr))) {
      perror("multicast:bind");
      exit(1);
    }
  }

  if (multicast_addr != NULL) {
    /* Subscribe to a well-known multicast IP channel. */
    mcChannel.imr_multiaddr.s_addr = inet_addr(multicast_addr);
    mcChannel.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(server->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		   &mcChannel, sizeof(mcChannel))) {
      perror("multicast:setsockopt(IP_ADD_MEMBERSHIP)");
      exit(1);
    }
    /* Record the multicast address so we can send messages in the future. */
    memset(&nanny_globals.udp_multicast_addr, 0, sizeof(nanny_globals.udp_multicast_addr));
    nanny_globals.udp_multicast_addr.sin_family = AF_INET;
    nanny_globals.udp_multicast_addr.sin_port = htons(port);
    nanny_globals.udp_multicast_addr.sin_addr.s_addr = inet_addr(multicast_addr);
  }

  /* Record the address and port in the server structure. */
  server->addr_len = sizeof(server->addr);
  if (getsockname(server->sock,
		  (struct sockaddr *)&server->addr, &server->addr_len))
    perror("getsockname");

  /* Record the unicast socket for future use. */
  if (multicast_addr == NULL)
    nanny_globals.udp_unicast_socket = server->sock;

  return (server);
}

static void
udp_server_message(void *_server)
{
  char buff[16384];
  struct sockaddr_in address;
  struct udp_server *server = _server;
  socklen_t len = sizeof(address);
  ssize_t bytes;

  memset(&address, 0, sizeof(address));
  bytes = recvfrom(server->sock, buff, sizeof(buff) - 1, 0,
		   (struct sockaddr *)&address, &len);
  if (bytes < 0) {
    perror("recvfrom");
    return;
  }

  buff[bytes] = 0;

  udp_query_response(buff, nanny_globals.udp_unicast_socket,
		     (struct sockaddr *)&address, len);
}

void
udp_server_init(const char *addr, int port)
{
  struct udp_server *u = udp_server(addr, port);
  nanny_register_server(udp_server_message, u->sock, u);
}
