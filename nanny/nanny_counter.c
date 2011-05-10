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
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nanny.h"

#define WORD_LEN 64

struct word {
  int  hash;
  intmax_t count;
  char text[WORD_LEN];
};

struct counter_server {
  int fd;
  char *path;
  char buff[2048];
  size_t buff_size;
  char *start;
  char *end;
  struct word words[8];
};

static unsigned int
hash(const char *p)
{
  unsigned int h = 0;
  unsigned int x = 0;

  while (*p != '\0') {
    h = (h << 4) + *(const unsigned char *)p;
    x = h & 0xF0000000L;
    if (x != 0) {
      h ^= (x >> 24);
      h &= ~x;
    }
    ++p;
  }
  return (h & 0x7FFFFFFFL);
}

static void
update(struct word *word, const char *p)
{
  ++word->count;
  printf("Word: %s, %jd\n", word->text, word->count);
}

static void
count_word(struct counter_server *server,
	   char *prefix_start, char *prefix_end, char *word, char *end)
{
  char buff[64];
  size_t len = (prefix_end - prefix_start) + (end - word);
  unsigned int h;
  int i;

  if (len < 1 || len > 63)
    return;
  if (prefix_start != NULL) {
    strlcpy(buff, prefix_start, sizeof(buff));
    strlcat(buff, word, sizeof(buff));
    word = buff;
  }

  h = hash(word);

  for (i = 0; i < 8; ++i) {
    if (server->words[i].hash == h) {
      if (strcmp(server->words[i].text, word) == 0) {
	update(&(server->words[i]), word);
	return;
      }
    }
    if (server->words[i].hash == 0 && server->words[i].text[0] == '\0') {
      strlcpy(server->words[i].text, word, WORD_LEN);
      server->words[i].hash = h;
      update(&(server->words[i]), word);
      return;
    }
  }
  printf("No space for word: %s\n", word);
}

static void
count_words(struct counter_server *server)
{
  char *p, *end, *w, *w0, *p0;

  /* Process a contiguous block of characters. */
  w0 = p0 = NULL;
  p = server->start;
  end = server->buff + server->buff_size;
  if (server->end == server->start)
    return;
  if (server->end > server->start)
    end = server->end;
  else
    end = server->buff + server->buff_size;

  /* Discard any leading whitespace (include all control chars as whitespace) */
  while (*p <= ' ' && p < end)
    ++p;

  for (;;) {
    w = server->start = p;
    /* Locate contiguous group of non-whitespace. */
    while (*p > ' ' && p < end)
      ++p;
    if (p != end) {
      /* Found contiguous word from w to p */
      *p = '\0';
      count_word(server, w0, p0, w, p);
      w0 = p0 = NULL;
      /* Skip whitespace before looking for next word. */
      while (*p <= ' ' && p < end)
	++p;
    } else {
      /* If we hit end of data, return. */
      if (p == server->end) {
	server->start = w0 != NULL ? w0 : w;
	return;
      }
      /* We hit end of buffer; wrap and keep going from start. */
      w0 = w; p0 = p;
      p = server->buff;
      end = server->end;
    }
  }
}

static void
counter_server_read(void *s0)
{
  struct counter_server *server = s0;
  ssize_t bytes;

  if (server->start <= server->end)
    bytes = server->buff + server->buff_size - server->end;
  else
    bytes = server->start - server->end - 1;

  bytes = read(server->fd, server->end, bytes);
  if (bytes < 0) {
    perror("read");
    return;
  }
  if (bytes == 0) { /* End-of-data terminates a word also. */
    server->end[0] = '\0';
    bytes = 1;
  }
  server->end += bytes;
  count_words(server);
  if (server->end == server->buff + server->buff_size)
    server->end = server->buff;
}

void *
nanny_counter_server_init(const char *pathname)
{
  struct counter_server *server;
  char tmpname[] = "/tmp/nanny_socket_XXXXXXXX";

  server = malloc(sizeof(struct counter_server));
  memset(server, 0, sizeof(*server));
  server->buff_size = sizeof(server->buff) - 1;
  server->buff[server->buff_size] = '\0';
  server->start = server->buff;
  server->end = server->buff;

  if (pathname == NULL) {
    mktemp(tmpname);
    server->path = strdup(tmpname);
  } else {
    server->path = strdup(pathname);
  }

  if (mkfifo(server->path, 0755)) {
    perror("mkfifo");
    free(server->path);
    free(server);
    return NULL;
  }

  server->fd = open(server->path, O_RDONLY | O_NONBLOCK);
  if (server->fd < 0) {
    perror("open fifo");
    free(server->path);
    free(server);
    return NULL;
  }

  nanny_register_server(counter_server_read, server->fd, server);
  return server;
}

void
nanny_counter_server_close(void *s0)
{
  struct counter_server *server = s0;

  if (server == NULL)
    return;

  close(server->fd);
  nanny_unregister_server(server->fd);
  unlink(server->path);
  free(server->path);
  free(server);
}
