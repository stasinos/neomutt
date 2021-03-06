/**
 * @file
 * Compressed mbox local mailbox type
 *
 * @authors
 * Copyright (C) 1997 Alain Penders <Alain@Finale-Dev.com>
 * Copyright (C) 2016 Richard Russon <rich@flatcap.org>
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

/**
 * @page compress Compressed mbox local mailbox type
 *
 * Compressed mbox local mailbox type
 *
 * @note
 * Any references to compressed files also apply to encrypted files.
 * - mailbox->path     == plaintext file
 * - mailbox->realpath == compressed file
 */

#include "config.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "config/lib.h"
#include "compress.h"
#include "context.h"
#include "curs_lib.h"
#include "format_flags.h"
#include "globals.h"
#include "hook.h"
#include "mailbox.h"
#include "mutt_curses.h"
#include "muttlib.h"
#include "mx.h"
#include "protos.h"

struct Email;

/**
 * struct CompressInfo - Private data for compress
 *
 * This object gets attached to the Mailbox.
 */
struct CompressInfo
{
  const char *append;            /**< append-hook command */
  const char *close;             /**< close-hook  command */
  const char *open;              /**< open-hook   command */
  long size;                     /**< size of the compressed file */
  const struct MxOps *child_ops; /**< callbacks of de-compressed file */
  bool locked;                   /**< if realpath is locked */
  FILE *lockfp;                  /**< fp used for locking */
};

/**
 * lock_realpath - Try to lock the ctx->realpath
 * @param mailbox Mailbox to lock
 * @param excl    Lock exclusively?
 * @retval true  Success (locked or readonly)
 * @retval false Error (can't lock the file)
 *
 * Try to (exclusively) lock the mailbox.  If we succeed, then we mark the
 * mailbox as locked.  If we fail, but we didn't want exclusive rights, then
 * the mailbox will be marked readonly.
 */
static bool lock_realpath(struct Mailbox *mailbox, bool excl)
{
  if (!mailbox)
    return false;

  struct CompressInfo *ci = mailbox->compress_info;
  if (!ci)
    return false;

  if (ci->locked)
    return true;

  if (excl)
    ci->lockfp = fopen(mailbox->realpath, "a");
  else
    ci->lockfp = fopen(mailbox->realpath, "r");
  if (!ci->lockfp)
  {
    mutt_perror(mailbox->realpath);
    return false;
  }

  int r = mutt_file_lock(fileno(ci->lockfp), excl, true);
  if (r == 0)
    ci->locked = true;
  else if (excl)
  {
    mutt_file_fclose(&ci->lockfp);
    mailbox->readonly = true;
    return true;
  }

  return r == 0;
}

/**
 * unlock_realpath - Unlock the mailbox->realpath
 * @param mailbox Mailbox to unlock
 *
 * Unlock a mailbox previously locked by lock_mailbox().
 */
static void unlock_realpath(struct Mailbox *mailbox)
{
  if (!mailbox)
    return;

  struct CompressInfo *ci = mailbox->compress_info;
  if (!ci)
    return;

  if (!ci->locked)
    return;

  mutt_file_unlock(fileno(ci->lockfp));

  ci->locked = false;
  mutt_file_fclose(&ci->lockfp);
}

/**
 * setup_paths - Set the mailbox paths
 * @param mailbox Mailbox to modify
 * @retval  0 Success
 * @retval -1 Error
 *
 * Save the compressed filename in mailbox->realpath.
 * Create a temporary filename and put its name in mailbox->path.
 * The temporary file is created to prevent symlink attacks.
 */
static int setup_paths(struct Mailbox *mailbox)
{
  if (!mailbox)
    return -1;

  char tmp[PATH_MAX];

  /* Setup the right paths */
  mutt_str_strfcpy(mailbox->realpath, mailbox->path, sizeof(mailbox->realpath));

  /* We will uncompress to /tmp */
  mutt_mktemp(tmp, sizeof(tmp));
  mutt_str_strfcpy(mailbox->path, tmp, sizeof(mailbox->path));

  FILE *fp = mutt_file_fopen(mailbox->path, "w");
  if (!fp)
    return -1;

  mutt_file_fclose(&fp);
  return 0;
}

/**
 * store_size - Save the size of the compressed file
 * @param mailbox Mailbox
 *
 * Save the compressed file size in the compress_info struct.
 */
static void store_size(const struct Mailbox *mailbox)
{
  if (!mailbox)
    return;

  struct CompressInfo *ci = mailbox->compress_info;
  if (!ci)
    return;

  ci->size = mutt_file_get_size(mailbox->realpath);
}

/**
 * find_hook - Find a hook to match a path
 * @param type Type of hook, e.g. #MUTT_CLOSE_HOOK
 * @param path Filename to test
 * @retval ptr  Matching hook command
 * @retval NULL No matches
 *
 * Each hook has a type and a pattern.
 * Find a command that matches the type and path supplied. e.g.
 *
 * User config:
 *      open-hook '\.gz$' "gzip -cd '%f' > '%t'"
 *
 * Call:
 *      find_hook (#MUTT_OPEN_HOOK, "myfile.gz");
 */
static const char *find_hook(int type, const char *path)
{
  if (!path)
    return NULL;

  const char *c = mutt_find_hook(type, path);
  if (!c || !*c)
    return NULL;

  return c;
}

/**
 * set_compress_info - Find the compress hooks for a mailbox
 * @param mailbox Mailbox to examine
 * @retval ptr  CompressInfo Hook info for the mailbox's path
 * @retval NULL Error
 *
 * When a mailbox is opened, we check if there are any matching hooks.
 */
static struct CompressInfo *set_compress_info(struct Mailbox *mailbox)
{
  if (!mailbox)
    return NULL;

  if (mailbox->compress_info)
    return mailbox->compress_info;

  /* Open is compulsory */
  const char *o = find_hook(MUTT_OPEN_HOOK, mailbox->path);
  if (!o)
    return NULL;

  const char *c = find_hook(MUTT_CLOSE_HOOK, mailbox->path);
  const char *a = find_hook(MUTT_APPEND_HOOK, mailbox->path);

  struct CompressInfo *ci = mutt_mem_calloc(1, sizeof(struct CompressInfo));
  mailbox->compress_info = ci;

  ci->open = mutt_str_strdup(o);
  ci->close = mutt_str_strdup(c);
  ci->append = mutt_str_strdup(a);

  return ci;
}

/**
 * free_compress_info - Frees the compress info members and structure
 * @param mailbox Mailbox to free compress_info for
 */
static void free_compress_info(struct Mailbox *mailbox)
{
  if (!mailbox || !mailbox->compress_info)
    return;

  struct CompressInfo *ci = mailbox->compress_info;
  FREE(&ci->open);
  FREE(&ci->close);
  FREE(&ci->append);

  unlock_realpath(mailbox);

  FREE(&mailbox->compress_info);
}

/**
 * escape_path - Escapes single quotes in a path for a command string
 * @param src the path to escape
 * @retval ptr The escaped string
 */
static char *escape_path(char *src)
{
  static char dest[HUGE_STRING];
  char *destp = dest;
  int destsize = 0;

  if (!src)
    return NULL;

  while (*src && (destsize < sizeof(dest) - 1))
  {
    if (*src != '\'')
    {
      *destp++ = *src++;
      destsize++;
    }
    else
    {
      /* convert ' into '\'' */
      if (destsize + 4 < sizeof(dest))
      {
        *destp++ = *src++;
        *destp++ = '\\';
        *destp++ = '\'';
        *destp++ = '\'';
        destsize += 4;
      }
      else
        break;
    }
  }
  *destp = '\0';

  return dest;
}

/**
 * compress_format_str - Expand the filenames in a command string - Implements ::format_t
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%f     | Compressed file
 * | \%t     | Plaintext, temporary file
 */
static const char *compress_format_str(char *buf, size_t buflen, size_t col, int cols,
                                       char op, const char *src, const char *prec,
                                       const char *if_str, const char *else_str,
                                       unsigned long data, enum FormatFlag flags)
{
  if (!buf || (data == 0))
    return src;

  struct Mailbox *mailbox = (struct Mailbox *) data;

  switch (op)
  {
    case 'f':
      /* Compressed file */
      snprintf(buf, buflen, "%s", NONULL(escape_path(mailbox->realpath)));
      break;
    case 't':
      /* Plaintext, temporary file */
      snprintf(buf, buflen, "%s", NONULL(escape_path(mailbox->path)));
      break;
  }
  return src;
}

/**
 * expand_command_str - Expand placeholders in command string
 * @param mailbox Mailbox for paths
 * @param cmd     Template command to be expanded
 * @param buf     Buffer to store the command
 * @param buflen  Size of the buffer
 *
 * This function takes a hook command and expands the filename placeholders
 * within it.  The function calls mutt_expando_format() to do the replacement
 * which calls our callback function compress_format_str(). e.g.
 *
 * Template command:
 *      gzip -cd '%f' > '%t'
 *
 * Result:
 *      gzip -dc '~/mail/abc.gz' > '/tmp/xyz'
 */
static void expand_command_str(const struct Mailbox *mailbox, const char *cmd,
                               char *buf, int buflen)
{
  if (!mailbox || !cmd || !buf)
    return;

  mutt_expando_format(buf, buflen, 0, buflen, cmd, compress_format_str,
                      (unsigned long) mailbox, 0);
}

/**
 * execute_command - Run a system command
 * @param mailbox  Mailbox to work with
 * @param command  Command string to execute
 * @param progress Message to show the user
 * @retval 1 Success
 * @retval 0 Failure
 *
 * Run the supplied command, taking care of all the NeoMutt requirements,
 * such as locking files and blocking signals.
 */
static int execute_command(struct Mailbox *mailbox, const char *command, const char *progress)
{
  int rc = 1;
  char sys_cmd[HUGE_STRING];

  if (!mailbox || !command || !progress)
    return 0;

  if (!mailbox->quiet)
  {
    mutt_message(progress, mailbox->realpath);
  }

  mutt_sig_block();
  endwin();
  fflush(stdout);

  expand_command_str(mailbox, command, sys_cmd, sizeof(sys_cmd));

  if (mutt_system(sys_cmd) != 0)
  {
    rc = 0;
    mutt_any_key_to_continue(NULL);
    mutt_error(_("Error running \"%s\""), sys_cmd);
  }

  mutt_sig_unblock();

  return rc;
}

/**
 * mutt_comp_can_append - Can we append to this path?
 * @param mailbox Mailbox
 * @retval true  Yes, we can append to the file
 * @retval false No, appending isn't possible
 *
 * To append to a file we can either use an 'append-hook' or a combination of
 * 'open-hook' and 'close-hook'.
 *
 * A match means it's our responsibility to append to the file.
 */
bool mutt_comp_can_append(struct Mailbox *mailbox)
{
  if (!mailbox)
    return false;

  /* If this succeeds, we know there's an open-hook */
  struct CompressInfo *ci = set_compress_info(mailbox);
  if (!ci)
    return false;

  /* We have an open-hook, so to append we need an append-hook,
   * or a close-hook. */
  if (ci->append || ci->close)
    return true;

  mutt_error(_("Cannot append without an append-hook or close-hook : %s"),
             mailbox->path);
  return false;
}

/**
 * mutt_comp_can_read - Can we read from this file?
 * @param path Pathname of file to be tested
 * @retval true  Yes, we can read the file
 * @retval false No, we cannot read the file
 *
 * Search for an 'open-hook' with a regex that matches the path.
 *
 * A match means it's our responsibility to open the file.
 */
bool mutt_comp_can_read(const char *path)
{
  if (!path)
    return false;

  if (find_hook(MUTT_OPEN_HOOK, path))
    return true;
  else
    return false;
}

/**
 * mutt_comp_valid_command - Is this command string allowed?
 * @param cmd  Command string
 * @retval 1 Valid command
 * @retval 0 "%f" and/or "%t" is missing
 *
 * A valid command string must have both "%f" (from file) and "%t" (to file).
 * We don't check if we can actually run the command.
 */
int mutt_comp_valid_command(const char *cmd)
{
  if (!cmd)
    return 0;

  return strstr(cmd, "%f") && strstr(cmd, "%t");
}

/**
 * comp_mbox_open - Implements MxOps::mbox_open()
 *
 * Set up a compressed mailbox to be read.
 * Decompress the mailbox and set up the paths and hooks needed.
 * Then determine the type of the mailbox so we can delegate the handling of
 * messages.
 */
static int comp_mbox_open(struct Context *ctx)
{
  if (!ctx || (ctx->mailbox->magic != MUTT_COMPRESSED))
    return -1;

  struct CompressInfo *ci = set_compress_info(ctx->mailbox);
  if (!ci)
    return -1;

  /* If there's no close-hook, or the file isn't writable */
  if (!ci->close || (access(ctx->mailbox->path, W_OK) != 0))
    ctx->mailbox->readonly = true;

  if (setup_paths(ctx->mailbox) != 0)
    goto cmo_fail;
  store_size(ctx->mailbox);

  if (!lock_realpath(ctx->mailbox, false))
  {
    mutt_error(_("Unable to lock mailbox"));
    goto cmo_fail;
  }

  int rc = execute_command(ctx->mailbox, ci->open, _("Decompressing %s"));
  if (rc == 0)
    goto cmo_fail;

  unlock_realpath(ctx->mailbox);

  ctx->mailbox->magic = mx_path_probe(ctx->mailbox->path, NULL);
  if (ctx->mailbox->magic == MUTT_UNKNOWN)
  {
    mutt_error(_("Can't identify the contents of the compressed file"));
    goto cmo_fail;
  }

  ci->child_ops = mx_get_ops(ctx->mailbox->magic);
  if (!ci->child_ops)
  {
    mutt_error(_("Can't find mailbox ops for mailbox type %d"), ctx->mailbox->magic);
    goto cmo_fail;
  }

  return ci->child_ops->mbox_open(ctx);

cmo_fail:
  /* remove the partial uncompressed file */
  remove(ctx->mailbox->path);
  free_compress_info(ctx->mailbox);
  return -1;
}

/**
 * comp_mbox_open_append - Implements MxOps::mbox_open_append()
 *
 * flags may also contain #MUTT_NEWFOLDER
 *
 * To append to a compressed mailbox we need an append-hook (or both open- and
 * close-hooks).
 */
static int comp_mbox_open_append(struct Context *ctx, int flags)
{
  if (!ctx)
    return -1;

  /* If this succeeds, we know there's an open-hook */
  struct CompressInfo *ci = set_compress_info(ctx->mailbox);
  if (!ci)
    return -1;

  /* To append we need an append-hook or a close-hook */
  if (!ci->append && !ci->close)
  {
    mutt_error(_("Cannot append without an append-hook or close-hook : %s"),
               ctx->mailbox->path);
    goto cmoa_fail1;
  }

  if (setup_paths(ctx->mailbox) != 0)
    goto cmoa_fail2;

  /* Lock the realpath for the duration of the append.
   * It will be unlocked in the close */
  if (!lock_realpath(ctx->mailbox, true))
  {
    mutt_error(_("Unable to lock mailbox"));
    goto cmoa_fail2;
  }

  /* Open the existing mailbox, unless we are appending */
  if (!ci->append && (mutt_file_get_size(ctx->mailbox->realpath) > 0))
  {
    int rc = execute_command(ctx->mailbox, ci->open, _("Decompressing %s"));
    if (rc == 0)
    {
      mutt_error(_("Compress command failed: %s"), ci->open);
      goto cmoa_fail2;
    }
    ctx->mailbox->magic = mx_path_probe(ctx->mailbox->path, NULL);
  }
  else
    ctx->mailbox->magic = MboxType;

  /* We can only deal with mbox and mmdf mailboxes */
  if ((ctx->mailbox->magic != MUTT_MBOX) && (ctx->mailbox->magic != MUTT_MMDF))
  {
    mutt_error(_("Unsupported mailbox type for appending"));
    goto cmoa_fail2;
  }

  ci->child_ops = mx_get_ops(ctx->mailbox->magic);
  if (!ci->child_ops)
  {
    mutt_error(_("Can't find mailbox ops for mailbox type %d"), ctx->mailbox->magic);
    goto cmoa_fail2;
  }

  if (ci->child_ops->mbox_open_append(ctx, flags) != 0)
    goto cmoa_fail2;

  return 0;

cmoa_fail2:
  /* remove the partial uncompressed file */
  remove(ctx->mailbox->path);
cmoa_fail1:
  /* Free the compress_info to prevent close from trying to recompress */
  free_compress_info(ctx->mailbox);

  return -1;
}

/**
 * comp_mbox_check - Implements MxOps::mbox_check()
 * @param ctx        Mailbox
 * @param index_hint Currently selected mailbox
 * @retval 0              Mailbox OK
 * @retval #MUTT_REOPENED The mailbox was closed and reopened
 * @retval -1             Mailbox bad
 *
 * If the compressed file changes in size but the mailbox hasn't been changed
 * in NeoMutt, then we can close and reopen the mailbox.
 *
 * If the mailbox has been changed in NeoMutt, warn the user.
 *
 * The return codes are picked to match mx_mbox_check().
 */
static int comp_mbox_check(struct Context *ctx, int *index_hint)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  int size = mutt_file_get_size(ctx->mailbox->realpath);
  if (size == ci->size)
    return 0;

  if (!lock_realpath(ctx->mailbox, false))
  {
    mutt_error(_("Unable to lock mailbox"));
    return -1;
  }

  int rc = execute_command(ctx->mailbox, ci->open, _("Decompressing %s"));
  store_size(ctx->mailbox);
  unlock_realpath(ctx->mailbox);
  if (rc == 0)
    return -1;

  return ops->mbox_check(ctx, index_hint);
}

/**
 * comp_mbox_sync - Implements MxOps::mbox_sync()
 *
 * Changes in NeoMutt only affect the tmp file.
 * Calling comp_mbox_sync() will commit them to the compressed file.
 */
static int comp_mbox_sync(struct Context *ctx, int *index_hint)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  if (!ci->close)
  {
    mutt_error(_("Can't sync a compressed file without a close-hook"));
    return -1;
  }

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  if (!lock_realpath(ctx->mailbox, true))
  {
    mutt_error(_("Unable to lock mailbox"));
    return -1;
  }

  int rc = comp_mbox_check(ctx, index_hint);
  if (rc != 0)
    goto sync_cleanup;

  rc = ops->mbox_sync(ctx, index_hint);
  if (rc != 0)
    goto sync_cleanup;

  rc = execute_command(ctx->mailbox, ci->close, _("Compressing %s"));
  if (rc == 0)
  {
    rc = -1;
    goto sync_cleanup;
  }

  rc = 0;

sync_cleanup:
  store_size(ctx->mailbox);
  unlock_realpath(ctx->mailbox);
  return rc;
}

/**
 * comp_mbox_close - Implements MxOps::mbox_close()
 *
 * If the mailbox has been changed then re-compress the tmp file.
 * Then delete the tmp file.
 */
static int comp_mbox_close(struct Context *ctx)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
  {
    free_compress_info(ctx->mailbox);
    return -1;
  }

  ops->mbox_close(ctx);

  /* sync has already been called, so we only need to delete some files */
  if (!ctx->append)
  {
    /* If the file was removed, remove the compressed folder too */
    if ((access(ctx->mailbox->path, F_OK) != 0) && !SaveEmpty)
    {
      remove(ctx->mailbox->realpath);
    }
    else
    {
      remove(ctx->mailbox->path);
    }
  }
  else
  {
    const char *append = NULL;
    const char *msg = NULL;

    /* The file exists and we can append */
    if ((access(ctx->mailbox->realpath, F_OK) == 0) && ci->append)
    {
      append = ci->append;
      msg = _("Compressed-appending to %s...");
    }
    else
    {
      append = ci->close;
      msg = _("Compressing %s...");
    }

    int rc = execute_command(ctx->mailbox, append, msg);
    if (rc == 0)
    {
      mutt_any_key_to_continue(NULL);
      mutt_error(_("Error. Preserving temporary file: %s"), ctx->mailbox->path);
    }
    else
      remove(ctx->mailbox->path);

    unlock_realpath(ctx->mailbox);
  }

  free_compress_info(ctx->mailbox);

  return 0;
}

/**
 * comp_msg_open - Implements MxOps::msg_open()
 */
static int comp_msg_open(struct Context *ctx, struct Message *msg, int msgno)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  /* Delegate */
  return ops->msg_open(ctx, msg, msgno);
}

/**
 * comp_msg_open_new - Implements MxOps::msg_open_new()
 */
static int comp_msg_open_new(struct Context *ctx, struct Message *msg, struct Email *e)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  /* Delegate */
  return ops->msg_open_new(ctx, msg, e);
}

/**
 * comp_msg_commit - Implements MxOps::msg_commit()
 */
static int comp_msg_commit(struct Context *ctx, struct Message *msg)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  /* Delegate */
  return ops->msg_commit(ctx, msg);
}

/**
 * comp_msg_close - Implements MxOps::msg_close()
 */
static int comp_msg_close(struct Context *ctx, struct Message *msg)
{
  if (!ctx)
    return -1;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return -1;

  const struct MxOps *ops = ci->child_ops;
  if (!ops)
    return -1;

  /* Delegate */
  return ops->msg_close(ctx, msg);
}

/**
 * comp_msg_padding_size - Bytes of padding between messages - Implements MxOps::msg_padding_size()
 * @param ctx Mailbox
 * @retval num Number of bytes of padding
 */
static int comp_msg_padding_size(struct Context *ctx)
{
  if (!ctx)
    return 0;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return 0;

  const struct MxOps *ops = ci->child_ops;
  if (!ops || !ops->msg_padding_size)
    return 0;

  return ops->msg_padding_size(ctx);
}

/**
 * comp_tags_edit - Implements MxOps::tags_edit()
 */
static int comp_tags_edit(struct Context *ctx, const char *tags, char *buf, size_t buflen)
{
  if (!ctx)
    return 0;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return 0;

  const struct MxOps *ops = ci->child_ops;
  if (!ops || !ops->tags_edit)
    return 0;

  return ops->tags_edit(ctx, tags, buf, buflen);
}

/**
 * comp_tags_commit - Implements MxOps::tags_commit()
 */
static int comp_tags_commit(struct Context *ctx, struct Email *e, char *buf)
{
  if (!ctx)
    return 0;

  struct CompressInfo *ci = ctx->mailbox->compress_info;
  if (!ci)
    return 0;

  const struct MxOps *ops = ci->child_ops;
  if (!ops || !ops->tags_commit)
    return 0;

  return ops->tags_commit(ctx, e, buf);
}

/**
 * comp_path_probe - Is this a compressed mailbox? - Implements MxOps::path_probe()
 */
int comp_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (!st || !S_ISREG(st->st_mode))
    return MUTT_UNKNOWN;

  if (mutt_comp_can_read(path))
    return MUTT_COMPRESSED;

  return MUTT_UNKNOWN;
}

/**
 * comp_path_canon - Canonicalise a mailbox path - Implements MxOps::path_canon()
 */
int comp_path_canon(char *buf, size_t buflen, const char *folder)
{
  if (!buf)
    return -1;

  if ((buf[0] == '+') || (buf[0] == '='))
  {
    if (!folder)
      return -1;

    buf[0] = '/';
    mutt_str_inline_replace(buf, buflen, 0, folder);
  }

  mutt_path_canon(buf, buflen, HomeDir);
  return 0;
}

/**
 * comp_path_pretty - Implements MxOps::path_pretty()
 */
int comp_path_pretty(char *buf, size_t buflen, const char *folder)
{
  if (!buf)
    return -1;

  if (mutt_path_abbr_folder(buf, buflen, folder))
    return 0;

  if (mutt_path_pretty(buf, buflen, HomeDir))
    return 0;

  return -1;
}

/**
 * comp_path_parent - Implements MxOps::path_parent()
 */
int comp_path_parent(char *buf, size_t buflen)
{
  if (!buf)
    return -1;

  if (mutt_path_parent(buf, buflen))
    return 0;

  if (buf[0] == '~')
    mutt_path_canon(buf, buflen, HomeDir);

  if (mutt_path_parent(buf, buflen))
    return 0;

  return -1;
}

// clang-format off
/**
 * struct mx_comp_ops - Compressed mailbox - Implements ::MxOps
 *
 * Compress only uses open, close and check.
 * The message functions are delegated to mbox.
 */
struct MxOps mx_comp_ops = {
  .magic            = MUTT_COMPRESSED,
  .name             = "compressed",
  .mbox_open        = comp_mbox_open,
  .mbox_open_append = comp_mbox_open_append,
  .mbox_check       = comp_mbox_check,
  .mbox_sync        = comp_mbox_sync,
  .mbox_close       = comp_mbox_close,
  .msg_open         = comp_msg_open,
  .msg_open_new     = comp_msg_open_new,
  .msg_commit       = comp_msg_commit,
  .msg_close        = comp_msg_close,
  .msg_padding_size = comp_msg_padding_size,
  .tags_edit        = comp_tags_edit,
  .tags_commit      = comp_tags_commit,
  .path_probe       = comp_path_probe,
  .path_canon       = comp_path_canon,
  .path_pretty      = comp_path_pretty,
  .path_parent      = comp_path_parent,
};
// clang-format on
