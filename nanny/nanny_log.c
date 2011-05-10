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
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "nanny.h"

struct nanny_log {
  int refcnt;
  char *filename_base;
  char *filename;
  int file_fd;
  time_t last_rotate;
  uintmax_t last_rotate_bytes;
  time_t last_rotate_check;

  uintmax_t total_bytes;
  uintmax_t read_count;
  uintmax_t error_count;
  float	bytes_per_second;
  time_t bps_last_update_time;
  uintmax_t bps_last_update_bytes;
  char *buff;
  size_t buff_size;
  char *buff_end;
  char *buffp;
};

/*
 * Used to tie stdout/stderr for a subprocess to a buffer that
 * receives the output of that fd.
 */
struct nanny_log_io {
  int fd;
  struct nanny_log *buff;
};

/*
 * Allocate and return a nanny_log
 */
struct nanny_log *
nanny_log_alloc(size_t buffsize)
{
  struct nanny_log *nlog;

  nlog = malloc(sizeof(*nlog));
  memset(nlog, 0, sizeof(*nlog));

  nlog->file_fd = -1;
  nlog->refcnt = 1;
  nlog->buff_size = buffsize;
  nlog->buff = malloc(nlog->buff_size);
  if (nlog->buff == NULL)
    nlog->buff_size = 0;
  else
    memset(nlog->buff, 0, nlog->buff_size);
  nlog->buff_end = nlog->buff + nlog->buff_size;
  nlog->buffp = nlog->buff;
  return nlog;
}

void
nanny_log_set_filename(struct nanny_log *nlog, const char *fmt, ...)
{
  char filename[512];
  va_list ap;

  free(nlog->filename_base);
  nlog->filename_base = NULL;
  free(nlog->filename);
  nlog->filename = NULL;

  if (fmt == NULL)
    return;

  va_start(ap, fmt);
  vsnprintf(filename, sizeof(filename), fmt, ap);
  va_end(ap);
  if ((nlog->filename_base = strdup(filename)) == NULL) {
      fprintf(stderr, "nanny_log_set_filename: strdup failure\n");
      exit(1);
  }

}


/*
 * Decide whether we need to rotate logs.
 *
 * XXX TODO: Set up a timer to go off once an hour and
 * proactively close the open log files.  The current code
 * leaves log files open until the next write, which is
 * less than ideal. XXX
 */
static void
nanny_log_rotate(struct nanny_log *nlog)
{
  char filename[1024];
  const char *p;
  struct tm *tm;
  time_t creation;
  int l;


  if (nlog->file_fd >= 0) {
    /* Last top-of-hour. */
    time_t last_hour = nanny_globals.now - (nanny_globals.now % 3600);

    /* Close the old log file if: */
    if (  /* Passed top of hour */
	(nlog->last_rotate > 0 && nlog->last_rotate < last_hour)
	|| /* Logged more than 10MB to this file. */
	(nlog->total_bytes - nlog->last_rotate_bytes > 1000000)
	  )
      {
	close(nlog->file_fd);
	nlog->file_fd = -1;
	free(nlog->filename);
	nlog->filename = NULL;
      }
  }

  /*
   * If there's no configured log dir, we can't log, so don't try.
   */
  if (nlog->filename_base == NULL)
    return;

  /* If there's no log, open a new one. */
  if (nlog->file_fd < 0) {
    /* Select a timestamp for the file. */
    /*
     * If there was an hour or minute boundary between the last write
     * and now, round the time to that boundary.  This makes the
     * filenames prettier.
     */
    creation = nanny_globals.now;
    if (nlog->last_rotate_check > 0) {
      if (creation - (creation % 3600) > nlog->last_rotate_check)
	creation -= creation % 3600;
      else if (creation - (creation % 60) > nlog->last_rotate_check)
	creation -= creation % 60;
    }
    /* Append a timestamp to the filename. */
    tm = gmtime(&creation);
    strlcpy(filename, nlog->filename_base, sizeof(filename));
    strlcat(filename, ".", sizeof(filename));
    strftime(filename + strlen(filename),
	     sizeof(filename) - strlen(filename) - 1,
	     "%Y-%m-%dT%H.%M.%S", tm);

    /* Open the new log file. */
    nlog->file_fd =
      open(filename, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    /* If it fails (because we're logging so much that we've overrun
       the rotation within a single second) try once more with
       microseconds. */
    if (nlog->file_fd < 0) {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      l = snprintf(filename + strlen(filename),
          sizeof(filename) - strlen(filename), ".%06d", tv.tv_usec);
      if (l == -1 || l >= (int)(sizeof(filename) - strlen(filename))) {
          fprintf(stderr, "nanny_log_rotate: snprintf truncation\n");
          exit(1);
      }

      nlog->file_fd =
	open(filename, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    }

    /* If we succeeded in opening a new file, record the new name and
     * update the symlink. */
    if (nlog->file_fd >= 0) {
      if ((nlog->filename = strdup(filename)) == NULL) {
          fprintf(stderr, "nanny_log_rotate: strdup failure\n");
          exit(1);
      }
      unlink(nlog->filename_base); /* Remove old symlink, if any. */
      p = strrchr(nlog->filename, '/');
      if (p != NULL)
	symlink(p + 1, nlog->filename_base);

      /* Record log stats as of the last rotation. */
      nlog->last_rotate = nanny_globals.now;
      nlog->last_rotate_bytes = nlog->total_bytes;
    }
  }

  nlog->last_rotate_check = nanny_globals.now;
}

/*
 * Bump the refcnt for a log buff.
 */
void
nanny_log_retain(struct nanny_log *nlog)
{
  ++nlog->refcnt;
}

/*
 * Decrement the refcnt, free storage if refcnt falls to zero.
 */
void
nanny_log_release(struct nanny_log *nlog)
{
  --nlog->refcnt;
  if (nlog->refcnt < 0)
    fprintf(stderr, "REFCNT ERROR!!!\n");
  if (nlog->refcnt == 0) {
    free(nlog->buff);
    nlog->buff = NULL;
    if (nlog->file_fd >= 0)
      close(nlog->file_fd);
    free(nlog->filename);
    free(nlog->filename_base);
    free(nlog);
  }
}

/*
 * Update the bps statistics counters.
 */
static void
nanny_log_update_statistics(struct nanny_log *nlog)
{
  if (nlog->bps_last_update_time == nanny_globals.now)
    return;

  if (nlog->bps_last_update_time == 0)
    nlog->bytes_per_second = 0.0;
  else
    /* TODO: a little smoothing may be in order here. */
    /* TODO: Create a function to read the bytes_per_second value
     * that applies a smoothing decay so that you can see the value
     * decay if no input occurs for a long time. */
    nlog->bytes_per_second =
      (nlog->total_bytes - nlog->bps_last_update_bytes)
      / (nanny_globals.now - nlog->bps_last_update_time);

  nlog->bps_last_update_time = nanny_globals.now;
  nlog->bps_last_update_bytes = nlog->total_bytes;
}

/*
 * Useful to write status/progress messages into the child's event log.
 */
void
nanny_log_printf(struct nanny_log *nlog, char *fmt, ...)
{
  char msg[8192];
  char *p;
  va_list ap;
  ptrdiff_t towrite;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  nanny_log_rotate(nlog);
  if (nlog->file_fd >= 0)
    write(nlog->file_fd, msg, strlen(msg));

  p = msg;
  while (*p != '\0') {
    towrite = strlen(p);
    if (towrite > nlog->buff_end - nlog->buffp)
      towrite = nlog->buff_end - nlog->buffp;
    memcpy(nlog->buffp, p, towrite);
    p += towrite;
    nlog->buffp += towrite;
    nlog->total_bytes += towrite;
    nlog->read_count += 1;
    if (nlog->buffp >= nlog->buff_end)
      nlog->buffp = nlog->buff;
  }
  nanny_log_update_statistics(nlog);
}

/*
 * Registered as a server so it gets select()-based read events
 * when data is available on the pipe.  The overhead of this
 * very simple circular buffer is extraordinarily low.
 * Note that if we read and hit the end of the buffer, we'll
 * get another select() event at the next poll, so we don't
 * need to bother trying to handle wrap around apart from
 * resetting the pointer.
 */
static void
nanny_log_input_server(void *_io)
{
  ssize_t bytesread;
  struct nanny_log_io *io = (struct nanny_log_io *)_io;
  struct nanny_log *nlog = io->buff;

  bytesread = read(io->fd, nlog->buffp, nlog->buff_end - nlog->buffp);
  if (bytesread == 0) {
    /* Close the fd */
    close(io->fd);
    nanny_unregister_server(io->fd);
    io->fd = 0;
    /* Release the buffer */
    nanny_log_release(nlog);
    io->buff = NULL;
    /* release the io structure */
    free(io);
    return;
  }
  if (bytesread < 0) {
    nlog->error_count += 1;
    if (errno == EINTR) /* Interrupted by some signal; try again later. */
      return;
    if (errno == EAGAIN) { /* No data available; try again later. */
      fprintf(stderr, "Bogus empty read on %d\n", io->fd);
      return;
    }
    fprintf(stderr, "Read Error %d on fd %d: %s\n",
		       errno, io->fd, strerror(errno));
    nanny_log_printf(nlog, "Read Error %d on fd %d: %s\n",
		       errno, io->fd, strerror(errno));
    return;
  }

  nanny_log_rotate(nlog);
  if (nlog->file_fd >= 0)
    write(nlog->file_fd, nlog->buffp, bytesread);

  nlog->buffp += bytesread;
  nlog->read_count += 1;
  nlog->total_bytes += bytesread;
  nanny_log_update_statistics(nlog);

  if (nlog->buffp >= nlog->buff_end)
    nlog->buffp = nlog->buff;
}

/*
 * Listen on the fd, put all read data into the log.
 */
void
nanny_log_from_fd(int fd, struct nanny_log *nlog)
{
  struct nanny_log_io *io;

  io = malloc(sizeof(*io));
  io->fd = fd;
  io->buff = nlog;
  ++nlog->refcnt;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  nanny_register_server(nanny_log_input_server, fd, io);
}

/*
 *
 * HTTP RESPONSE GENERATION
 *
 * Functions that report log data.
 */

/*
 * Dump the contents of a nanny_log structure into an HTTP response.
 */
void
nanny_log_http_dump_raw(struct http_request *request, struct nanny_log *nlog)
{
  const char *p;

  /* Emit from buffp -> buff end */
  p = nlog->buffp;
  while (p < nlog->buff_end) {
    if (*p != '\0')
      http_printf(request, "%c", *p);
    ++p;
  }

  /* Emit from buff start -> buffp */
  p = nlog->buff;
  while (p < nlog->buffp) {
    if (*p != '\0')
      http_printf(request, "%c", *p);
    ++p;
  }
}

/*
 * Correctly quote a single character into a JSON response.
 */
static void
http_status_buff_char(struct http_request *request, char c,
		      int *lines, int *chars)
{
  const char *indent = "       ";
  if (c == '\0')
    return;

  if (*chars == 0) {
    if (*lines > 0)
      http_printf(request, "\",\n");
    http_printf(request, "%s\"", indent);
  }

  if (c == '\n') {
    ++(*lines);
    *chars = 0;
    return;
  }

  switch (c) {
  case '"': case '\\':  http_printf(request, "\\%c", c); break;
  case '\b':            http_printf(request, "\\b"); break;
  case '\f':            http_printf(request, "\\f"); break;
  case '\n':            http_printf(request, "\\n"); break;
  case '\r':            http_printf(request, "\\r"); break;
  case '\t':            http_printf(request, "\t"); break; /* Let tab through. */
  default:
    if (c >= 32 && c < 127)
      http_printf(request, "%c", c);
    else
      http_printf(request, "\\u%04X", 0xff & c);
  }
  ++(*chars);
}

/*
 * Dump a log buffer contents into a JSON output.
 */
void
nanny_log_http_dump_json(struct http_request *request,
			 struct nanny_log *nlog,
			 const char *name,
			 const char *indent)
{
  const char *p;
  int lines, chars;

  http_printf(request, "%s\"%s\": {\n", indent, name);
  if (nlog->filename_base)
    http_printf(request, "%s  \"filename_base\": \"%s\",\n",
		indent, nlog->filename_base);
  if (nlog->filename)
    http_printf(request, "%s  \"filename\": \"%s\",\n",
		indent, nlog->filename);
  http_printf(request, "%s  \"total_bytes\": %jd,\n",
	      indent, nlog->total_bytes);
  http_printf(request, "%s  \"read_count\": %d,\n",
	      indent, nlog->read_count);
  http_printf(request, "%s  \"error_count\": %d,\n",
	      indent, nlog->error_count);
  http_printf(request, "%s  \"bytes_per_second\": %f,\n",
	      indent, nlog->bytes_per_second);
  http_printf(request, "%s  \"lines\": [\n", indent);

  /* Emit from buffp -> buff end */
  p = nlog->buffp;
  lines = chars = 0;
  while (p < nlog->buff_end) {
    http_status_buff_char(request, *p, &lines, &chars);
    ++p;
  }

  /* Emit from buff start -> buffp */
  p = nlog->buff;
  while (p < nlog->buffp) {
    http_status_buff_char(request, *p, &lines, &chars);
    ++p;
  }
  if (chars > 0 || lines > 0)
    http_printf(request, "\"\n"); /* Finish the unfinished line. */

  http_printf(request, "%s  ]\n", indent);
  http_printf(request, "%s}\n", indent);
}
