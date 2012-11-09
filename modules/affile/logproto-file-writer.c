/*
 * Copyright (c) 2002-2012 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2012 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "logproto-file-writer.h"
#include "messages.h"

#include <string.h>
#include <sys/uio.h>

typedef struct _LogProtoFileWriter
{
  LogProtoClient super;
  guchar *partial;
  gsize partial_len, partial_pos;
  gint buf_size;
  gint buf_count;
  gint fd;
  gint sum_len;
  gboolean fsync;
  struct iovec buffer[0];
} LogProtoFileWriter;

/*
 * log_proto_file_writer_flush:
 *
 * this function flushes the file output buffer
 * it is called either form log_proto_file_writer_post (normal mode: the buffer is full)
 * or from log_proto_flush (foced flush: flush time, exit, etc)
 *
 */
static LogProtoStatus
log_proto_file_writer_flush(LogProtoClient *s)
{
  LogProtoFileWriter *self = (LogProtoFileWriter *)s;
  gint rc, i, i0, sum, ofs, pos;

  /* we might be called from log_writer_deinit() without having a buffer at all */

  if (self->buf_count == 0)
    return LPS_SUCCESS;

  /* lseek() is used instead of O_APPEND, as on NFS  O_APPEND performs
   * poorly, as reported on the mailing list 2008/05/29 */

  lseek(self->fd, 0, SEEK_END);
  rc = writev(self->fd, self->buffer, self->buf_count);
  if (rc > 0 && self->fsync)
    fsync(self->fd);

  if (rc < 0)
    {
      if (errno != EAGAIN && errno != EINTR)
        {
          msg_error("I/O error occurred while writing",
                    evt_tag_int("fd", self->super.transport->fd),
                    evt_tag_errno(EVT_TAG_OSERROR, errno),
                    NULL);
          return LPS_ERROR;
        }

      return LPS_SUCCESS;
    }
  else if (rc != self->sum_len)
    {
      /* partial success: not everything has been written out */
      /* look for the first chunk that has been cut */
      sum = self->buffer[0].iov_len; /* sum is the cumulated length of the already processed items */
      i = 0;
      while (rc > sum)
        sum += self->buffer[++i].iov_len;
      self->partial_len = sum - rc; /* this is the length of the first non-written chunk */
      i0 = i;
      ++i;
      /* add the lengths of the following messages */
      while (i < self->buf_count)
        self->partial_len += self->buffer[i++].iov_len;
      /* allocate and copy the remaning data */
      self->partial = (guchar *)g_malloc(self->partial_len);
      ofs = sum - rc; /* the length of the remaning (not processed) chunk in the first message */
      pos = self->buffer[i0].iov_len - ofs;
      memcpy(self->partial, self->buffer[i0].iov_base + pos, ofs);
      i = i0 + 1;
      while (i < self->buf_count)
        {
          memcpy(self->partial + ofs, self->buffer[i].iov_base, self->buffer[i].iov_len);
          ofs += self->buffer[i].iov_len;
          ++i;
        }
      self->partial_pos = 0;
    }

  /* free the previous message strings (the remaning part has been copied to the partial buffer) */
  for (i = 0; i < self->buf_count; ++i)
    g_free(self->buffer[i].iov_base);
  self->buf_count = 0;
  self->sum_len = 0;

  return LPS_SUCCESS;
}

/*
 * log_proto_file_writer_post:
 * @msg: formatted log message to send (this might be consumed by this function)
 * @msg_len: length of @msg
 * @consumed: pointer to a gboolean that gets set if the message was consumed by this function
 * @error: error information, if any
 *
 * This function posts a message to the log transport, performing buffering
 * of partially sent data if needed. The return value indicates whether we
 * successfully sent this message, or if it should be resent by the caller.
 **/
static LogProtoStatus
log_proto_file_writer_post(LogProtoClient *s, guchar *msg, gsize msg_len, gboolean *consumed)
{
  LogProtoFileWriter *self = (LogProtoFileWriter *)s;
  gint rc;

  if (self->buf_count >= self->buf_size)
    {
      rc = log_proto_file_writer_flush(s);
      if (rc != LPS_SUCCESS || self->buf_count >= self->buf_size)
        {
          /* don't consume a new message if flush failed, or even after the flush we don't have any free slots */
          return rc;
        }
    }

  *consumed = FALSE;
  if (self->partial)
    {
      /* there is still some data from the previous file writing process */
      gint len = self->partial_len - self->partial_pos;

      rc = write(self->fd, self->partial + self->partial_pos, len);
      if (rc > 0 && self->fsync)
        fsync(self->fd);
      if (rc < 0)
        {
          goto write_error;
        }
      else if (rc != len)
        {
          self->partial_pos += rc;
          return LPS_SUCCESS;
        }
      else
        {
          g_free(self->partial);
          self->partial = NULL;
          /* NOTE: we return here to give a chance to the framed protocol to send the frame header. */
          return LPS_SUCCESS;
        }
    }

  /* register the new message */
  self->buffer[self->buf_count].iov_base = (void *) msg;
  self->buffer[self->buf_count].iov_len = msg_len;
  ++self->buf_count;
  self->sum_len += msg_len;
  *consumed = TRUE;

  if (self->buf_count == self->buf_size)
    {
      /* we have reached the max buffer size -> we need to write the messages */
      return log_proto_file_writer_flush(s);
    }

  return LPS_SUCCESS;

write_error:
  if (errno != EAGAIN && errno != EINTR)
    {
      msg_error("I/O error occurred while writing",
                evt_tag_int("fd", self->super.transport->fd),
                evt_tag_errno(EVT_TAG_OSERROR, errno),
                NULL);
      return LPS_ERROR;
    }

  return LPS_SUCCESS;
}

static gboolean
log_proto_file_writer_prepare(LogProtoClient *s, gint *fd, GIOCondition *cond)
{
  LogProtoFileWriter *self = (LogProtoFileWriter *) s;

  *fd = self->super.transport->fd;
  *cond = self->super.transport->cond;

  /* if there's no pending I/O in the transport layer, then we want to do a write */
  if (*cond == 0)
    *cond = G_IO_OUT;
  return self->buf_count > 0 || self->partial;
}

LogProtoClient *
log_proto_file_writer_new(LogTransport *transport, const LogProtoClientOptions *options, gint fsync, gint flush_lines)
{
  if (flush_lines == 0)
    /* the flush-lines option has not been specified, use a default value */
    flush_lines = 1;
#ifdef IOV_MAX
  if (flush_lines > IOV_MAX)
    /* limit the flush_lines according to the current platform */
    flush_lines = IOV_MAX;
#endif

  /* allocate the structure with the proper number of items at the end */
  LogProtoFileWriter *self = (LogProtoFileWriter *)g_malloc0(sizeof(LogProtoFileWriter) + sizeof(struct iovec)*flush_lines);

  log_proto_client_init(&self->super, transport, options);
  self->fd = transport->fd;
  self->buf_size = flush_lines;
  self->fsync = fsync;
  self->super.prepare = log_proto_file_writer_prepare;
  self->super.post = log_proto_file_writer_post;
  self->super.flush = log_proto_file_writer_flush;
  return &self->super;
}
