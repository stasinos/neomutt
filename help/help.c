/**
 * @file
 * Help system
 *
 * @authors
 * Copyright (C) 2018 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <stddef.h>
#include <stdbool.h>
#include "mutt/mutt.h"
#include "mx.h"

struct Context;
struct Header;
struct Message;

int help_mbox_open(struct Context *ctx)
{
  mutt_debug(1, "entering help_mbox_open\n");
  return -1;
}

int help_mbox_open_append(struct Context *ctx, int flags)
{
  mutt_debug(1, "entering help_mbox_open_append\n");
  return -1;
}

int help_mbox_check(struct Context *ctx, int *index_hint)
{
  mutt_debug(1, "entering help_mbox_check\n");
  return -1;
}

int help_mbox_sync(struct Context *ctx, int *index_hint)
{
  mutt_debug(1, "entering help_mbox_sync\n");
  return -1;
}

int help_mbox_close(struct Context *ctx)
{
  mutt_debug(1, "entering help_mbox_close\n");
  return -1;
}

int help_msg_open(struct Context *ctx, struct Message *msg, int msgno)
{
  mutt_debug(1, "entering help_msg_open\n");
  return -1;
}

int help_msg_open_new(struct Context *ctx, struct Message *msg, struct Email *e)
{
  mutt_debug(1, "entering help_msg_open_new\n");
  return -1;
}

int help_msg_commit(struct Context *ctx, struct Message *msg)
{
  mutt_debug(1, "entering help_msg_commit\n");
  return -1;
}

int help_msg_close(struct Context *ctx, struct Message *msg)
{
  mutt_debug(1, "entering help_msg_close\n");
  return -1;
}

int help_msg_padding_size(struct Context *ctx)
{
  mutt_debug(1, "entering help_msg_padding_size\n");
  return -1;
}

int help_tags_edit(struct Context *ctx, const char *tags, char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_tags_edit\n");
  return -1;
}

int help_tags_commit(struct Context *ctx, struct Email *e, char *buf)
{
  mutt_debug(1, "entering help_tags_commit\n");
  return -1;
}

int help_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (mutt_str_strncasecmp(path, "help://", 7) == 0)
    return MUTT_HELP;

  return MUTT_UNKNOWN;
}

int help_path_canon(char *buf, size_t buflen, const char *folder)
{
  mutt_debug(1, "entering help_path_canon\n");
  return -1;
}

int help_path_pretty(char *buf, size_t buflen, const char *folder)
{
  mutt_debug(1, "entering help_path_pretty\n");
  return -1;
}

int help_path_parent(char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_path_parent\n");
  return -1;
}

// clang-format off
/**
 * mx_help_ops - Help Mailbox callback functions
 */
struct MxOps mx_help_ops = {
  .magic            = MUTT_HELP,
  .name             = "help",
  .mbox_open        = help_mbox_open,
  .mbox_open_append = help_mbox_open_append,
  .mbox_check       = help_mbox_check,
  .mbox_sync        = help_mbox_sync,
  .mbox_close       = help_mbox_close,
  .msg_open         = help_msg_open,
  .msg_open_new     = help_msg_open_new,
  .msg_commit       = help_msg_commit,
  .msg_close        = help_msg_close,
  .msg_padding_size = help_msg_padding_size,
  .tags_edit        = help_tags_edit,
  .tags_commit      = help_tags_commit,
  .path_probe       = help_path_probe,
  .path_canon       = help_path_canon,
  .path_pretty      = help_path_pretty,
  .path_parent      = help_path_parent,
};
// clang-format on
