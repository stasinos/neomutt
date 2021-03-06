/**
 * @file
 * Maildir/MH local mailbox type
 *
 * @authors
 * Copyright (C) 1996-2002,2007,2009 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2005 Thomas Roessler <roessler@does-not-exist.org>
 * Copyright (C) 2010,2013 Michael R. Elkins <me@mutt.org>
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
 * @page maildir_maildir Maildir/MH local mailbox type
 *
 * Maildir/MH local mailbox type
 */

#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "mutt/mutt.h"
#include "email/lib.h"
#include "mutt.h"
#include "context.h"
#include "copy.h"
#include "globals.h"
#include "mailbox.h"
#include "maildir.h"
#include "mutt_thread.h"
#include "muttlib.h"
#include "mx.h"
#include "progress.h"
#include "protos.h"
#include "sort.h"
#ifdef USE_NOTMUCH
#include "notmuch/mutt_notmuch.h"
#endif
#ifdef USE_HCACHE
#include "hcache/hcache.h"
#endif
#ifdef USE_INOTIFY
#include "monitor.h"
#endif

/* These Config Variables are only used in maildir/mh.c */
bool CheckNew; ///< Config: (maildir,mh) Check for new mail while the mailbox is open
bool MaildirHeaderCacheVerify; ///< Config: (hcache) Check for maildir changes when opening mailbox
bool MhPurge;       ///< Config: Really delete files in MH mailboxes
char *MhSeqFlagged; ///< Config: MH sequence for flagged message
char *MhSeqReplied; ///< Config: MH sequence to tag replied messages
char *MhSeqUnseen;  ///< Config: MH sequence for unseen messages

#define INS_SORT_THRESHOLD 6

#define MH_SEQ_UNSEEN (1 << 0)
#define MH_SEQ_REPLIED (1 << 1)
#define MH_SEQ_FLAGGED (1 << 2)

/**
 * struct Maildir - A Maildir mailbox
 */
struct Maildir
{
  struct Email *email;
  char *canon_fname;
  bool header_parsed : 1;
  ino_t inode;
  struct Maildir *next;
};

/**
 * struct MhSequences - Set of MH sequence numbers
 */
struct MhSequences
{
  int max;
  short *flags;
};

/**
 * struct MaildirMboxData - Maildir-specific mailbox data
 */
struct MaildirMboxData
{
  struct timespec mtime_cur;
  mode_t mh_umask;
};

/**
 * maildir_get_mdata - Get the private data for this Mailbox
 * @param m Mailbox
 * @retval ptr MaildirMboxData
 */
static struct MaildirMboxData *maildir_get_mdata(struct Mailbox *m)
{
  if (!m || ((m->magic != MUTT_MAILDIR) && (m->magic != MUTT_MH)))
    return NULL;
  return m->data;
}

/**
 * mhs_alloc - Allocate more memory for sequences
 * @param mhs Existing sequences
 * @param i   Number required
 *
 * @note Memory is allocated in blocks of 128.
 */
static void mhs_alloc(struct MhSequences *mhs, int i)
{
  if ((i <= mhs->max) && mhs->flags)
    return;

  const int newmax = i + 128;
  int j = mhs->flags ? mhs->max + 1 : 0;
  mutt_mem_realloc(&mhs->flags, sizeof(mhs->flags[0]) * (newmax + 1));
  while (j <= newmax)
    mhs->flags[j++] = 0;

  mhs->max = newmax;
}

/**
 * mhs_free_sequences - Free some sequences
 * @param mhs Sequences to free
 */
static void mhs_free_sequences(struct MhSequences *mhs)
{
  FREE(&mhs->flags);
}

/**
 * mhs_check - Get the flags for a given sequence
 * @param mhs Sequences
 * @param i   Index number required
 * @retval num Flags, e.g. #MH_SEQ_UNSEEN
 */
static short mhs_check(struct MhSequences *mhs, int i)
{
  if (!mhs->flags || (i > mhs->max))
    return 0;
  else
    return mhs->flags[i];
}

/**
 * mhs_set - Set a flag for a given sequence
 * @param mhs Sequences
 * @param i   Index number
 * @param f   Flags, e.g. #MH_SEQ_UNSEEN
 * @retval num Resulting flags
 */
static short mhs_set(struct MhSequences *mhs, int i, short f)
{
  mhs_alloc(mhs, i);
  mhs->flags[i] |= f;
  return mhs->flags[i];
}

/**
 * mh_read_token - Parse a number, or number range
 * @param t     String to parse
 * @param first First number
 * @param last  Last number (if a range, first number if not)
 * @retval  0 Success
 * @retval -1 Error
 */
static int mh_read_token(char *t, int *first, int *last)
{
  char *p = strchr(t, '-');
  if (p)
  {
    *p++ = '\0';
    if ((mutt_str_atoi(t, first) < 0) || (mutt_str_atoi(p, last) < 0))
      return -1;
  }
  else
  {
    if (mutt_str_atoi(t, first) < 0)
      return -1;
    *last = *first;
  }
  return 0;
}

/**
 * mh_read_sequences - Read a set of MH sequences
 * @param mhs  Existing sequences
 * @param path File to read from
 * @retval  0 Success
 * @retval -1 Error
 */
static int mh_read_sequences(struct MhSequences *mhs, const char *path)
{
  int line = 1;
  char *buf = NULL;
  size_t sz = 0;

  short f;
  int first, last, rc = 0;

  char pathname[PATH_MAX];
  snprintf(pathname, sizeof(pathname), "%s/.mh_sequences", path);

  FILE *fp = fopen(pathname, "r");
  if (!fp)
    return 0; /* yes, ask callers to silently ignore the error */

  while ((buf = mutt_file_read_line(buf, &sz, fp, &line, 0)))
  {
    char *t = strtok(buf, " \t:");
    if (!t)
      continue;

    if (mutt_str_strcmp(t, MhSeqUnseen) == 0)
      f = MH_SEQ_UNSEEN;
    else if (mutt_str_strcmp(t, MhSeqFlagged) == 0)
      f = MH_SEQ_FLAGGED;
    else if (mutt_str_strcmp(t, MhSeqReplied) == 0)
      f = MH_SEQ_REPLIED;
    else /* unknown sequence */
      continue;

    while ((t = strtok(NULL, " \t:")))
    {
      if (mh_read_token(t, &first, &last) < 0)
      {
        mhs_free_sequences(mhs);
        rc = -1;
        goto out;
      }
      for (; first <= last; first++)
        mhs_set(mhs, first, f);
    }
  }

  rc = 0;

out:
  FREE(&buf);
  mutt_file_fclose(&fp);
  return rc;
}

/**
 * mh_umask - Create a umask from the mailbox directory
 * @param  mailbox Mailbox
 * @retval num     Umask
 */
static inline mode_t mh_umask(struct Mailbox *mailbox)
{
  struct MaildirMboxData *mdata = maildir_get_mdata(mailbox);
  if (mdata && mdata->mh_umask)
    return mdata->mh_umask;

  struct stat st;
  if (stat(mailbox->path, &st))
  {
    mutt_debug(1, "stat failed on %s\n", mailbox->path);
    return 077;
  }

  return 0777 & ~st.st_mode;
}

/**
 * mh_sequences_changed - Has the mailbox changed
 * @param m Mailbox
 * @retval 1 mh_sequences last modification time is more recent than the last visit to this mailbox
 * @retval 0 modification time is older
 * @retval -1 Error
 */
static int mh_sequences_changed(struct Mailbox *m)
{
  char path[PATH_MAX];
  struct stat sb;

  if ((snprintf(path, sizeof(path), "%s/.mh_sequences", m->path) < sizeof(path)) &&
      (stat(path, &sb) == 0))
  {
    return (mutt_stat_timespec_compare(&sb, MUTT_STAT_MTIME, &m->last_visited) > 0);
  }
  return -1;
}

/**
 * mh_already_notified - Has the message changed
 * @param m     Mailbox
 * @param msgno Message number
 * @retval 1 Modification time on the message file is older than the last visit to this mailbox
 * @retval 0 Modification time on the message file is newer
 * @retval -1 Error
 */
static int mh_already_notified(struct Mailbox *m, int msgno)
{
  char path[PATH_MAX];
  struct stat sb;

  if ((snprintf(path, sizeof(path), "%s/%d", m->path, msgno) < sizeof(path)) &&
      (stat(path, &sb) == 0))
  {
    return (mutt_stat_timespec_compare(&sb, MUTT_STAT_MTIME, &m->last_visited) <= 0);
  }
  return -1;
}

/**
 * mh_valid_message - Is this a valid MH message filename
 * @param s Pathname to examine
 * @retval true name is valid
 * @retval false name is invalid
 *
 * Ignore the garbage files.  A valid MH message consists of only
 * digits.  Deleted message get moved to a filename with a comma before
 * it.
 */
static bool mh_valid_message(const char *s)
{
  for (; *s; s++)
  {
    if (!isdigit((unsigned char) *s))
      return false;
  }
  return true;
}

/**
 * mh_mkstemp - Create a temporary file
 * @param[in]  mailbox Mailbox to create the file in
 * @param[out] fp      File handle
 * @param[out] tgt     File name
 * @retval  0 Success
 * @retval -1 Failure
 */
static int mh_mkstemp(struct Mailbox *mailbox, FILE **fp, char **tgt)
{
  int fd;
  char path[PATH_MAX];

  mode_t omask = umask(mh_umask(mailbox));
  while (true)
  {
    snprintf(path, sizeof(path), "%s/.neomutt-%s-%d-%" PRIu64, mailbox->path,
             NONULL(ShortHostname), (int) getpid(), mutt_rand64());
    fd = open(path, O_WRONLY | O_EXCL | O_CREAT, 0666);
    if (fd == -1)
    {
      if (errno != EEXIST)
      {
        mutt_perror(path);
        umask(omask);
        return -1;
      }
    }
    else
    {
      *tgt = mutt_str_strdup(path);
      break;
    }
  }
  umask(omask);

  *fp = fdopen(fd, "w");
  if (!*fp)
  {
    FREE(tgt);
    close(fd);
    unlink(path);
    return -1;
  }

  return 0;
}

/**
 * mhs_write_one_sequence - Write a flag sequence to a file
 * @param fp  File to write to
 * @param mhs Sequence list
 * @param f   Flag, e.g. MH_SEQ_UNSEEN
 * @param tag string tag, e.g. "unseen"
 */
static void mhs_write_one_sequence(FILE *fp, struct MhSequences *mhs, short f, const char *tag)
{
  fprintf(fp, "%s:", tag);

  int first = -1;
  int last = -1;

  for (int i = 0; i <= mhs->max; i++)
  {
    if ((mhs_check(mhs, i) & f))
    {
      if (first < 0)
        first = i;
      else
        last = i;
    }
    else if (first >= 0)
    {
      if (last < 0)
        fprintf(fp, " %d", first);
      else
        fprintf(fp, " %d-%d", first, last);

      first = -1;
      last = -1;
    }
  }

  if (first >= 0)
  {
    if (last < 0)
      fprintf(fp, " %d", first);
    else
      fprintf(fp, " %d-%d", first, last);
  }

  fputc('\n', fp);
}

/**
 * mh_update_sequences - Update sequence numbers
 * @param mailbox Mailbox
 *
 * XXX we don't currently remove deleted messages from sequences we don't know.
 * Should we?
 */
static void mh_update_sequences(struct Mailbox *mailbox)
{
  FILE *ofp = NULL, *nfp = NULL;

  char sequences[PATH_MAX];
  char *tmpfname = NULL;
  char *buf = NULL;
  char *p = NULL;
  size_t s;
  int l = 0;
  int i;

  int unseen = 0;
  int flagged = 0;
  int replied = 0;

  char seq_unseen[STRING];
  char seq_replied[STRING];
  char seq_flagged[STRING];

  struct MhSequences mhs = { 0 };

  snprintf(seq_unseen, sizeof(seq_unseen), "%s:", NONULL(MhSeqUnseen));
  snprintf(seq_replied, sizeof(seq_replied), "%s:", NONULL(MhSeqReplied));
  snprintf(seq_flagged, sizeof(seq_flagged), "%s:", NONULL(MhSeqFlagged));

  if (mh_mkstemp(mailbox, &nfp, &tmpfname) != 0)
  {
    /* error message? */
    return;
  }

  snprintf(sequences, sizeof(sequences), "%s/.mh_sequences", mailbox->path);

  /* first, copy unknown sequences */
  ofp = fopen(sequences, "r");
  if (ofp)
  {
    while ((buf = mutt_file_read_line(buf, &s, ofp, &l, 0)))
    {
      if (mutt_str_strncmp(buf, seq_unseen, mutt_str_strlen(seq_unseen)) == 0)
        continue;
      if (mutt_str_strncmp(buf, seq_flagged, mutt_str_strlen(seq_flagged)) == 0)
        continue;
      if (mutt_str_strncmp(buf, seq_replied, mutt_str_strlen(seq_replied)) == 0)
        continue;

      fprintf(nfp, "%s\n", buf);
    }
  }
  mutt_file_fclose(&ofp);

  /* now, update our unseen, flagged, and replied sequences */
  for (l = 0; l < mailbox->msg_count; l++)
  {
    if (mailbox->hdrs[l]->deleted)
      continue;

    p = strrchr(mailbox->hdrs[l]->path, '/');
    if (p)
      p++;
    else
      p = mailbox->hdrs[l]->path;

    if (mutt_str_atoi(p, &i) < 0)
      continue;

    if (!mailbox->hdrs[l]->read)
    {
      mhs_set(&mhs, i, MH_SEQ_UNSEEN);
      unseen++;
    }
    if (mailbox->hdrs[l]->flagged)
    {
      mhs_set(&mhs, i, MH_SEQ_FLAGGED);
      flagged++;
    }
    if (mailbox->hdrs[l]->replied)
    {
      mhs_set(&mhs, i, MH_SEQ_REPLIED);
      replied++;
    }
  }

  /* write out the new sequences */
  if (unseen)
    mhs_write_one_sequence(nfp, &mhs, MH_SEQ_UNSEEN, NONULL(MhSeqUnseen));
  if (flagged)
    mhs_write_one_sequence(nfp, &mhs, MH_SEQ_FLAGGED, NONULL(MhSeqFlagged));
  if (replied)
    mhs_write_one_sequence(nfp, &mhs, MH_SEQ_REPLIED, NONULL(MhSeqReplied));

  mhs_free_sequences(&mhs);

  /* try to commit the changes - no guarantee here */
  mutt_file_fclose(&nfp);

  unlink(sequences);
  if (mutt_file_safe_rename(tmpfname, sequences) != 0)
  {
    /* report an error? */
    unlink(tmpfname);
  }

  FREE(&tmpfname);
}

/**
 * mh_sequences_add_one - Update the flags for one sequence
 * @param mailbox Mailbox
 * @param n       Sequence number to update
 * @param unseen  Update the unseen sequence
 * @param flagged Update the flagged sequence
 * @param replied Update the replied sequence
 */
static void mh_sequences_add_one(struct Mailbox *mailbox, int n, bool unseen,
                                 bool flagged, bool replied)
{
  bool unseen_done = false;
  bool flagged_done = false;
  bool replied_done = false;

  FILE *ofp = NULL, *nfp = NULL;

  char *tmpfname = NULL;
  char sequences[PATH_MAX];

  char seq_unseen[STRING];
  char seq_replied[STRING];
  char seq_flagged[STRING];

  char *buf = NULL;
  int line = 0;
  size_t sz;

  if (mh_mkstemp(mailbox, &nfp, &tmpfname) == -1)
    return;

  snprintf(seq_unseen, sizeof(seq_unseen), "%s:", NONULL(MhSeqUnseen));
  snprintf(seq_replied, sizeof(seq_replied), "%s:", NONULL(MhSeqReplied));
  snprintf(seq_flagged, sizeof(seq_flagged), "%s:", NONULL(MhSeqFlagged));

  snprintf(sequences, sizeof(sequences), "%s/.mh_sequences", mailbox->path);
  ofp = fopen(sequences, "r");
  if (ofp)
  {
    while ((buf = mutt_file_read_line(buf, &sz, ofp, &line, 0)))
    {
      if (unseen && (strncmp(buf, seq_unseen, mutt_str_strlen(seq_unseen)) == 0))
      {
        fprintf(nfp, "%s %d\n", buf, n);
        unseen_done = true;
      }
      else if (flagged && (strncmp(buf, seq_flagged, mutt_str_strlen(seq_flagged)) == 0))
      {
        fprintf(nfp, "%s %d\n", buf, n);
        flagged_done = true;
      }
      else if (replied && (strncmp(buf, seq_replied, mutt_str_strlen(seq_replied)) == 0))
      {
        fprintf(nfp, "%s %d\n", buf, n);
        replied_done = true;
      }
      else
        fprintf(nfp, "%s\n", buf);
    }
  }
  mutt_file_fclose(&ofp);
  FREE(&buf);

  if (!unseen_done && unseen)
    fprintf(nfp, "%s: %d\n", NONULL(MhSeqUnseen), n);
  if (!flagged_done && flagged)
    fprintf(nfp, "%s: %d\n", NONULL(MhSeqFlagged), n);
  if (!replied_done && replied)
    fprintf(nfp, "%s: %d\n", NONULL(MhSeqReplied), n);

  mutt_file_fclose(&nfp);

  unlink(sequences);
  if (mutt_file_safe_rename(tmpfname, sequences) != 0)
    unlink(tmpfname);

  FREE(&tmpfname);
}

/**
 * mh_update_maildir - Update our record of flags
 * @param md  Maildir to update
 * @param mhs Sequences
 */
static void mh_update_maildir(struct Maildir *md, struct MhSequences *mhs)
{
  int i;

  for (; md; md = md->next)
  {
    char *p = strrchr(md->email->path, '/');
    if (p)
      p++;
    else
      p = md->email->path;

    if (mutt_str_atoi(p, &i) < 0)
      continue;
    short f = mhs_check(mhs, i);

    md->email->read = (f & MH_SEQ_UNSEEN) ? false : true;
    md->email->flagged = (f & MH_SEQ_FLAGGED) ? true : false;
    md->email->replied = (f & MH_SEQ_REPLIED) ? true : false;
  }
}

/**
 * maildir_free_entry - Free a Maildir object
 * @param md Maildir to free
 */
static void maildir_free_entry(struct Maildir **md)
{
  if (!md || !*md)
    return;

  FREE(&(*md)->canon_fname);
  if ((*md)->email)
    mutt_email_free(&(*md)->email);

  FREE(md);
}

/**
 * maildir_free_maildir - Free a Maildir list
 * @param md Maildir list to free
 */
static void maildir_free_maildir(struct Maildir **md)
{
  if (!md || !*md)
    return;

  struct Maildir *p = NULL, *q = NULL;

  for (p = *md; p; p = q)
  {
    q = p->next;
    maildir_free_entry(&p);
  }
}

/**
 * maildir_update_mtime - Update our record of the Maildir modification time
 * @param mailbox Mailbox
 */
static void maildir_update_mtime(struct Mailbox *mailbox)
{
  char buf[PATH_MAX];
  struct stat st;
  struct MaildirMboxData *mdata = maildir_get_mdata(mailbox);

  if (mailbox->magic == MUTT_MAILDIR)
  {
    snprintf(buf, sizeof(buf), "%s/%s", mailbox->path, "cur");
    if (stat(buf, &st) == 0)
      mutt_get_stat_timespec(&mdata->mtime_cur, &st, MUTT_STAT_MTIME);
    snprintf(buf, sizeof(buf), "%s/%s", mailbox->path, "new");
  }
  else
  {
    snprintf(buf, sizeof(buf), "%s/.mh_sequences", mailbox->path);
    if (stat(buf, &st) == 0)
      mutt_get_stat_timespec(&mdata->mtime_cur, &st, MUTT_STAT_MTIME);

    mutt_str_strfcpy(buf, mailbox->path, sizeof(buf));
  }

  if (stat(buf, &st) == 0)
    mutt_get_stat_timespec(&mailbox->mtime, &st, MUTT_STAT_MTIME);
}

/**
 * maildir_parse_dir - Read a Maildir mailbox
 * @param mailbox  Mailbox
 * @param last     Last Maildir
 * @param subdir   Subdirectory, e.g. 'new'
 * @param count    Counter for the progress bar
 * @param progress Progress bar
 * @retval  0 Success
 * @retval -1 Error
 * @retval -2 Aborted
 */
static int maildir_parse_dir(struct Mailbox *mailbox, struct Maildir ***last,
                             const char *subdir, int *count, struct Progress *progress)
{
  struct dirent *de = NULL;
  int rc = 0, is_old = 0;
  struct Maildir *entry = NULL;
  struct Email *e = NULL;

  struct Buffer *buf = mutt_buffer_pool_get();

  if (subdir)
  {
    mutt_buffer_printf(buf, "%s/%s", mailbox->path, subdir);
    is_old = MarkOld ? (mutt_str_strcmp("cur", subdir) == 0) : false;
  }
  else
    mutt_buffer_strcpy(buf, mailbox->path);

  DIR *dirp = opendir(mutt_b2s(buf));
  if (!dirp)
  {
    rc = -1;
    goto cleanup;
  }

  while (((de = readdir(dirp))) && (SigInt != 1))
  {
    if (((mailbox->magic == MUTT_MH) && !mh_valid_message(de->d_name)) ||
        ((mailbox->magic == MUTT_MAILDIR) && (*de->d_name == '.')))
    {
      continue;
    }

    /* FOO - really ignore the return value? */
    mutt_debug(2, "queueing %s\n", de->d_name);

    e = mutt_email_new();
    e->old = is_old;
    if (mailbox->magic == MUTT_MAILDIR)
      maildir_parse_flags(e, de->d_name);

    if (count)
    {
      (*count)++;
      if (!mailbox->quiet && progress)
        mutt_progress_update(progress, *count, -1);
    }

    if (subdir)
    {
      mutt_buffer_printf(buf, "%s/%s", subdir, de->d_name);
      e->path = mutt_str_strdup(mutt_b2s(buf));
    }
    else
      e->path = mutt_str_strdup(de->d_name);

    entry = mutt_mem_calloc(1, sizeof(struct Maildir));
    entry->email = e;
    entry->inode = de->d_ino;
    **last = entry;
    *last = &entry->next;
  }

  closedir(dirp);

  if (SigInt == 1)
  {
    SigInt = 0;
    return -2; /* action aborted */
  }

cleanup:
  mutt_buffer_pool_release(&buf);

  return rc;
}

/**
 * maildir_add_to_context - Add the Maildir list to the Mailbox
 * @param ctx Mailbox
 * @param md  Maildir list to copy
 * @retval true If there's new mail
 */
static bool maildir_add_to_context(struct Context *ctx, struct Maildir *md)
{
  int oldmsgcount = ctx->mailbox->msg_count;

  if (!ctx->mailbox->hdrs)
  {
    /* Allocate some memory to get started */
    ctx->mailbox->hdrmax = ctx->mailbox->msg_count;
    ctx->mailbox->msg_count = 0;
    ctx->mailbox->vcount = 0;
    mx_alloc_memory(ctx->mailbox);
  }

  while (md)
  {
    mutt_debug(2, "Considering %s\n", NONULL(md->canon_fname));

    if (md->email)
    {
      mutt_debug(2, "Adding header structure. Flags: %s%s%s%s%s\n",
                 md->email->flagged ? "f" : "", md->email->deleted ? "D" : "",
                 md->email->replied ? "r" : "", md->email->old ? "O" : "",
                 md->email->read ? "R" : "");
      if (ctx->mailbox->msg_count == ctx->mailbox->hdrmax)
        mx_alloc_memory(ctx->mailbox);

      ctx->mailbox->hdrs[ctx->mailbox->msg_count] = md->email;
      ctx->mailbox->hdrs[ctx->mailbox->msg_count]->index = ctx->mailbox->msg_count;
      ctx->mailbox->size += md->email->content->length + md->email->content->offset -
                            md->email->content->hdr_offset;

      md->email = NULL;
      ctx->mailbox->msg_count++;
    }
    md = md->next;
  }

  if (ctx->mailbox->msg_count > oldmsgcount)
  {
    mx_update_context(ctx, ctx->mailbox->msg_count - oldmsgcount);
    return true;
  }
  return false;
}

/**
 * maildir_move_to_context - Copy the Maildir list to the Mailbox
 * @param ctx Mailbox
 * @param md  Maildir list to copy, then free
 * @retval 1 If there's new mail
 * @retval 0 Otherwise
 */
static int maildir_move_to_context(struct Context *ctx, struct Maildir **md)
{
  int r = maildir_add_to_context(ctx, *md);
  maildir_free_maildir(md);
  return r;
}

#ifdef USE_HCACHE
/**
 * maildir_hcache_keylen - Calculate the length of the Maildir path
 * @param fn File name
 * @retval size_t Length in bytes
 *
 * @note This length excludes the flags, which will vary
 */
static size_t maildir_hcache_keylen(const char *fn)
{
  const char *p = strrchr(fn, ':');
  return p ? (size_t)(p - fn) : mutt_str_strlen(fn);
}
#endif

/**
 * md_cmp_inode - Compare two Maildirs by inode number
 * @param a First  Maildir
 * @param b Second Maildir
 * @retval -1 a precedes b
 * @retval  0 a and b are identical
 * @retval  1 b precedes a
 */
static int md_cmp_inode(struct Maildir *a, struct Maildir *b)
{
  return a->inode - b->inode;
}

/**
 * md_cmp_path - Compare two Maildirs by path
 * @param a First  Maildir
 * @param b Second Maildir
 * @retval -1 a precedes b
 * @retval  0 a and b are identical
 * @retval  1 b precedes a
 */
static int md_cmp_path(struct Maildir *a, struct Maildir *b)
{
  return strcmp(a->email->path, b->email->path);
}

/**
 * maildir_merge_lists - Merge two maildir lists
 * @param left    First  Maildir to merge
 * @param right   Second Maildir to merge
 * @param cmp     Comparison function for sorting
 * @retval ptr Merged Maildir
 */
static struct Maildir *maildir_merge_lists(struct Maildir *left, struct Maildir *right,
                                           int (*cmp)(struct Maildir *, struct Maildir *))
{
  struct Maildir *head = NULL;
  struct Maildir *tail = NULL;

  if (left && right)
  {
    if (cmp(left, right) < 0)
    {
      head = left;
      left = left->next;
    }
    else
    {
      head = right;
      right = right->next;
    }
  }
  else
  {
    if (left)
      return left;
    else
      return right;
  }

  tail = head;

  while (left && right)
  {
    if (cmp(left, right) < 0)
    {
      tail->next = left;
      left = left->next;
    }
    else
    {
      tail->next = right;
      right = right->next;
    }
    tail = tail->next;
  }

  if (left)
  {
    tail->next = left;
  }
  else
  {
    tail->next = right;
  }

  return head;
}

/**
 * maildir_ins_sort - Sort maildirs using an insertion sort
 * @param list    Maildir list to sort
 * @param cmp     Comparison function for sorting
 * @retval ptr Sort Maildir list
 */
static struct Maildir *maildir_ins_sort(struct Maildir *list,
                                        int (*cmp)(struct Maildir *, struct Maildir *))
{
  struct Maildir *tmp = NULL, *last = NULL, *back = NULL;

  struct Maildir *ret = list;
  list = list->next;
  ret->next = NULL;

  while (list)
  {
    last = NULL;
    back = list->next;
    for (tmp = ret; tmp && cmp(tmp, list) <= 0; tmp = tmp->next)
      last = tmp;

    list->next = tmp;
    if (last)
      last->next = list;
    else
      ret = list;

    list = back;
  }

  return ret;
}

/**
 * maildir_sort - Sort Maildir list
 * @param list    Maildirs to sort
 * @param len     Number of Maildirs in the list
 * @param cmp     Comparison function for sorting
 * @retval ptr Sort Maildir list
 */
static struct Maildir *maildir_sort(struct Maildir *list, size_t len,
                                    int (*cmp)(struct Maildir *, struct Maildir *))
{
  struct Maildir *left = list;
  struct Maildir *right = list;
  size_t c = 0;

  if (!list || !list->next)
  {
    return list;
  }

  if (len != (size_t)(-1) && len <= INS_SORT_THRESHOLD)
    return maildir_ins_sort(list, cmp);

  list = list->next;
  while (list && list->next)
  {
    right = right->next;
    list = list->next->next;
    c++;
  }

  list = right;
  right = right->next;
  list->next = 0;

  left = maildir_sort(left, c, cmp);
  right = maildir_sort(right, c, cmp);
  return maildir_merge_lists(left, right, cmp);
}

/**
 * mh_sort_natural - Sort a Maildir list into its natural order
 * @param mailbox Mailbox
 * @param md      Maildir list to sort
 *
 * Currently only defined for MH where files are numbered.
 */
static void mh_sort_natural(struct Mailbox *mailbox, struct Maildir **md)
{
  if (!mailbox || !md || !*md || (mailbox->magic != MUTT_MH) || (Sort != SORT_ORDER))
    return;
  mutt_debug(4, "maildir: sorting %s into natural order\n", mailbox->path);
  *md = maildir_sort(*md, (size_t) -1, md_cmp_path);
}

/**
 * skip_duplicates - Find the first non-duplicate message
 * @param[in]  p    Maildir to start
 * @param[out] last Previous Maildir
 * @retval ptr Next Maildir
 */
static struct Maildir *skip_duplicates(struct Maildir *p, struct Maildir **last)
{
  /* Skip ahead to the next non-duplicate message.
   *
   * p should never reach NULL, because we couldn't have reached this point
   * unless there was a message that needed to be parsed.
   *
   * The check for p->header_parsed is likely unnecessary since the dupes will
   * most likely be at the head of the list.  but it is present for consistency
   * with the check at the top of the for() loop in maildir_delayed_parsing().
   */
  while (!p->email || p->header_parsed)
  {
    *last = p;
    p = p->next;
  }
  return p;
}

/**
 * maildir_delayed_parsing - This function does the second parsing pass
 * @param mailbox  Mailbox
 * @param md       Maildir to parse
 * @param progress Progress bar
 */
static void maildir_delayed_parsing(struct Mailbox *mailbox, struct Maildir **md,
                                    struct Progress *progress)
{
  struct Maildir *p, *last = NULL;
  char fn[PATH_MAX];
  int count;
  bool sort = false;
#ifdef USE_HCACHE
  const char *key = NULL;
  size_t keylen;
  struct stat lastchanged;
  int ret;
#endif

#ifdef USE_HCACHE
  header_cache_t *hc = mutt_hcache_open(HeaderCache, mailbox->path, NULL);
#endif

  for (p = *md, count = 0; p; p = p->next, count++)
  {
    if (!(p && p->email && !p->header_parsed))
    {
      last = p;
      continue;
    }

    if (!mailbox->quiet && progress)
      mutt_progress_update(progress, count, -1);

    if (!sort)
    {
      mutt_debug(4, "maildir: need to sort %s by inode\n", mailbox->path);
      p = maildir_sort(p, (size_t) -1, md_cmp_inode);
      if (!last)
        *md = p;
      else
        last->next = p;
      sort = true;
      p = skip_duplicates(p, &last);
      snprintf(fn, sizeof(fn), "%s/%s", mailbox->path, p->email->path);
    }

    snprintf(fn, sizeof(fn), "%s/%s", mailbox->path, p->email->path);

#ifdef USE_HCACHE
    if (MaildirHeaderCacheVerify)
    {
      ret = stat(fn, &lastchanged);
    }
    else
    {
      lastchanged.st_mtime = 0;
      ret = 0;
    }

    if (mailbox->magic == MUTT_MH)
    {
      key = p->email->path;
      keylen = strlen(key);
    }
    else
    {
      key = p->email->path + 3;
      keylen = maildir_hcache_keylen(key);
    }
    void *data = mutt_hcache_fetch(hc, key, keylen);
    struct timeval *when = data;

    if (data && !ret && lastchanged.st_mtime <= when->tv_sec)
    {
      struct Email *e = mutt_hcache_restore((unsigned char *) data);
      e->old = p->email->old;
      e->path = mutt_str_strdup(p->email->path);
      mutt_email_free(&p->email);
      p->email = e;
      if (mailbox->magic == MUTT_MAILDIR)
        maildir_parse_flags(p->email, fn);
    }
    else
    {
#endif

      if (maildir_parse_message(mailbox->magic, fn, p->email->old, p->email))
      {
        p->header_parsed = 1;
#ifdef USE_HCACHE
        if (mailbox->magic == MUTT_MH)
        {
          key = p->email->path;
          keylen = strlen(key);
        }
        else
        {
          key = p->email->path + 3;
          keylen = maildir_hcache_keylen(key);
        }
        mutt_hcache_store(hc, key, keylen, p->email, 0);
#endif
      }
      else
        mutt_email_free(&p->email);
#ifdef USE_HCACHE
    }
    mutt_hcache_free(hc, &data);
#endif
    last = p;
  }
#ifdef USE_HCACHE
  mutt_hcache_close(hc);
#endif

  mh_sort_natural(mailbox, md);
}

/**
 * mh_read_dir - Read a MH/maildir style mailbox
 * @param ctx    Mailbox
 * @param subdir NULL for MH mailboxes,
 *               otherwise the subdir of the maildir mailbox to read from
 * @retval  0 Success
 * @retval -1 Failure
 */
static int mh_read_dir(struct Context *ctx, const char *subdir)
{
  struct Maildir *md = NULL;
  struct MhSequences mhs = { 0 };
  struct Maildir **last = NULL;
  char msgbuf[STRING];
  struct Progress progress;

  if (!ctx->mailbox->quiet)
  {
    snprintf(msgbuf, sizeof(msgbuf), _("Scanning %s..."), ctx->mailbox->path);
    mutt_progress_init(&progress, msgbuf, MUTT_PROGRESS_MSG, ReadInc, 0);
  }

  if (!ctx->mailbox->data)
  {
    ctx->mailbox->data = mutt_mem_calloc(1, sizeof(struct MaildirMboxData));
  }
  struct MaildirMboxData *mdata = maildir_get_mdata(ctx->mailbox);

  maildir_update_mtime(ctx->mailbox);

  md = NULL;
  last = &md;
  int count = 0;
  if (maildir_parse_dir(ctx->mailbox, &last, subdir, &count, &progress) < 0)
    return -1;

  if (!ctx->mailbox->quiet)
  {
    snprintf(msgbuf, sizeof(msgbuf), _("Reading %s..."), ctx->mailbox->path);
    mutt_progress_init(&progress, msgbuf, MUTT_PROGRESS_MSG, ReadInc, count);
  }
  maildir_delayed_parsing(ctx->mailbox, &md, &progress);

  if (ctx->mailbox->magic == MUTT_MH)
  {
    if (mh_read_sequences(&mhs, ctx->mailbox->path) < 0)
    {
      maildir_free_maildir(&md);
      return -1;
    }
    mh_update_maildir(md, &mhs);
    mhs_free_sequences(&mhs);
  }

  maildir_move_to_context(ctx, &md);

  if (!mdata->mh_umask)
    mdata->mh_umask = mh_umask(ctx->mailbox);

  return 0;
}

/**
 * maildir_read_dir - Read a Maildir style mailbox
 * @param ctx Mailbox
 * @retval  0 Success
 * @retval -1 Failure
 */
static int maildir_read_dir(struct Context *ctx)
{
  /* maildir looks sort of like MH, except that there are two subdirectories
   * of the main folder path from which to read messages
   */
  if ((mh_read_dir(ctx, "new") == -1) || (mh_read_dir(ctx, "cur") == -1))
    return -1;

  return 0;
}

/**
 * ch_compare - qsort callback to sort characters
 * @param a First  character to compare
 * @param b Second character to compare
 * @retval -1 a precedes b
 * @retval  0 a and b are identical
 * @retval  1 b precedes a
 */
static int ch_compare(const void *a, const void *b)
{
  return (int) (*((const char *) a) - *((const char *) b));
}

/**
 * maildir_mh_open_message - Open a Maildir or MH message
 * @param mailbox    Mailbox
 * @param msg        Message to open
 * @param msgno      Index number
 * @param is_maildir true, if a Maildir
 * @retval  0 Success
 * @retval -1 Failure
 */
static int maildir_mh_open_message(struct Mailbox *mailbox, struct Message *msg,
                                   int msgno, int is_maildir)
{
  struct Email *cur = mailbox->hdrs[msgno];
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s/%s", mailbox->path, cur->path);

  msg->fp = fopen(path, "r");
  if (!msg->fp && (errno == ENOENT) && is_maildir)
    msg->fp = maildir_open_find_message(mailbox->path, cur->path, NULL);

  if (!msg->fp)
  {
    mutt_perror(path);
    mutt_debug(1, "fopen: %s: %s (errno %d).\n", path, strerror(errno), errno);
    return -1;
  }

  return 0;
}

/**
 * mh_commit_msg - Commit a message to an MH folder
 * @param mailbox Mailbox
 * @param msg     Message to commit
 * @param e     Email Header
 * @param updseq  If true, update the sequence number
 * @retval  0 Success
 * @retval -1 Failure
 */
static int mh_commit_msg(struct Mailbox *mailbox, struct Message *msg,
                         struct Email *e, bool updseq)
{
  struct dirent *de = NULL;
  char *cp = NULL, *dep = NULL;
  unsigned int n, hi = 0;
  char path[PATH_MAX];
  char tmp[16];

  if (mutt_file_fsync_close(&msg->fp))
  {
    mutt_perror(_("Could not flush message to disk"));
    return -1;
  }

  DIR *dirp = opendir(mailbox->path);
  if (!dirp)
  {
    mutt_perror(mailbox->path);
    return -1;
  }

  /* figure out what the next message number is */
  while ((de = readdir(dirp)))
  {
    dep = de->d_name;
    if (*dep == ',')
      dep++;
    cp = dep;
    while (*cp)
    {
      if (!isdigit((unsigned char) *cp))
        break;
      cp++;
    }
    if (!*cp)
    {
      n = atoi(dep);
      if (n > hi)
        hi = n;
    }
  }
  closedir(dirp);

  /* Now try to rename the file to the proper name.
   *
   * Note: We may have to try multiple times, until we find a free slot.
   */

  while (true)
  {
    hi++;
    snprintf(tmp, sizeof(tmp), "%u", hi);
    snprintf(path, sizeof(path), "%s/%s", mailbox->path, tmp);
    if (mutt_file_safe_rename(msg->path, path) == 0)
    {
      if (e)
        mutt_str_replace(&e->path, tmp);
      mutt_str_replace(&msg->committed_path, path);
      FREE(&msg->path);
      break;
    }
    else if (errno != EEXIST)
    {
      mutt_perror(mailbox->path);
      return -1;
    }
  }
  if (updseq)
  {
    mh_sequences_add_one(mailbox, hi, !msg->flags.read, msg->flags.flagged,
                         msg->flags.replied);
  }
  return 0;
}

/**
 * md_commit_message - Commit a message to a maildir folder
 * @param mailbox Mailbox
 * @param msg     Message to commit
 * @param e     Email
 * @retval  0 Success
 * @retval -1 Failure
 *
 * msg->path contains the file name of a file in tmp/. We take the
 * flags from this file's name.
 *
 * mailbox is the mail folder we commit to.
 *
 * e is a header structure to which we write the message's new
 * file name.  This is used in the mh and maildir folder synch
 * routines.  When this routine is invoked from mx_msg_commit(),
 * e is NULL.
 *
 * msg->path looks like this:
 *
 *    tmp/{cur,new}.neomutt-HOSTNAME-PID-COUNTER:flags
 *
 * See also maildir_msg_open_new().
 */
static int md_commit_message(struct Mailbox *mailbox, struct Message *msg, struct Email *e)
{
  char subdir[4];
  char suffix[16];
  int rc = 0;

  if (mutt_file_fsync_close(&msg->fp))
  {
    mutt_perror(_("Could not flush message to disk"));
    return -1;
  }

  /* extract the subdir */
  char *s = strrchr(msg->path, '/') + 1;
  mutt_str_strfcpy(subdir, s, 4);

  /* extract the flags */
  s = strchr(s, ':');
  if (s)
    mutt_str_strfcpy(suffix, s, sizeof(suffix));
  else
    suffix[0] = '\0';

  /* construct a new file name. */
  struct Buffer *path = mutt_buffer_pool_get();
  struct Buffer *full = mutt_buffer_pool_get();
  while (true)
  {
    mutt_buffer_printf(path, "%s/%lld.R%" PRIu64 ".%s%s", subdir, (long long) time(NULL),
                       mutt_rand64(), NONULL(ShortHostname), suffix);
    mutt_buffer_printf(full, "%s/%s", mailbox->path, mutt_b2s(path));

    mutt_debug(2, "renaming %s to %s.\n", msg->path, mutt_b2s(full));

    if (mutt_file_safe_rename(msg->path, mutt_b2s(full)) == 0)
    {
      /* Adjust the mtime on the file to match the time at which this
       * message was received.  Currently this is only set when copying
       * messages between mailboxes, so we test to ensure that it is
       * actually set.
       */
      if (msg->received)
      {
        struct utimbuf ut;

        ut.actime = msg->received;
        ut.modtime = msg->received;
        if (utime(mutt_b2s(full), &ut))
        {
          mutt_perror(_("md_commit_message(): unable to set time on file"));
          rc = -1;
          goto cleanup;
        }
      }

#ifdef USE_NOTMUCH
      if (mailbox->magic == MUTT_NOTMUCH)
        nm_update_filename(mailbox, e->path, mutt_b2s(full), e);
#endif
      if (e)
        mutt_str_replace(&e->path, mutt_b2s(path));
      mutt_str_replace(&msg->committed_path, mutt_b2s(full));
      FREE(&msg->path);

      goto cleanup;
    }
    else if (errno != EEXIST)
    {
      mutt_perror(mailbox->path);
      rc = -1;
      goto cleanup;
    }
  }

cleanup:
  mutt_buffer_pool_release(&path);
  mutt_buffer_pool_release(&full);

  return rc;
}

/**
 * mh_rewrite_message - Sync a message in an MH folder
 * @param ctx   Mailbox
 * @param msgno Index number
 * @retval  0 Success
 * @retval -1 Error
 *
 * This code is also used for attachment deletion in maildir folders.
 */
static int mh_rewrite_message(struct Context *ctx, int msgno)
{
  struct Email *e = ctx->mailbox->hdrs[msgno];
  bool restore = true;

  long old_body_offset = e->content->offset;
  long old_body_length = e->content->length;
  long old_hdr_lines = e->lines;

  struct Message *dest = mx_msg_open_new(ctx, e, 0);
  if (!dest)
    return -1;

  int rc = mutt_copy_message_ctx(dest->fp, ctx, e, MUTT_CM_UPDATE, CH_UPDATE | CH_UPDATE_LEN);
  if (rc == 0)
  {
    char oldpath[PATH_MAX];
    char partpath[PATH_MAX];
    snprintf(oldpath, sizeof(oldpath), "%s/%s", ctx->mailbox->path, e->path);
    mutt_str_strfcpy(partpath, e->path, sizeof(partpath));

    if (ctx->mailbox->magic == MUTT_MAILDIR)
      rc = md_commit_message(ctx->mailbox, dest, e);
    else
      rc = mh_commit_msg(ctx->mailbox, dest, e, false);

    mx_msg_close(ctx, &dest);

    if (rc == 0)
    {
      unlink(oldpath);
      restore = false;
    }

    /* Try to move the new message to the old place.
     * (MH only.)
     *
     * This is important when we are just updating flags.
     *
     * Note that there is a race condition against programs which
     * use the first free slot instead of the maximum message
     * number.  NeoMutt does _not_ behave like this.
     *
     * Anyway, if this fails, the message is in the folder, so
     * all what happens is that a concurrently running neomutt will
     * lose flag modifications.
     */

    if (ctx->mailbox->magic == MUTT_MH && rc == 0)
    {
      char newpath[PATH_MAX];
      snprintf(newpath, sizeof(newpath), "%s/%s", ctx->mailbox->path, e->path);
      rc = mutt_file_safe_rename(newpath, oldpath);
      if (rc == 0)
        mutt_str_replace(&e->path, partpath);
    }
  }
  else
    mx_msg_close(ctx, &dest);

  if (rc == -1 && restore)
  {
    e->content->offset = old_body_offset;
    e->content->length = old_body_length;
    e->lines = old_hdr_lines;
  }

  mutt_body_free(&e->content->parts);
  return rc;
}

/**
 * mh_sync_message - Sync an email to an MH folder
 * @param ctx   Mailbox
 * @param msgno Index number
 * @retval  0 Success
 * @retval -1 Error
 */
static int mh_sync_message(struct Context *ctx, int msgno)
{
  struct Email *e = ctx->mailbox->hdrs[msgno];

  if (e->attach_del || e->xlabel_changed ||
      (e->env && (e->env->refs_changed || e->env->irt_changed)))
  {
    if (mh_rewrite_message(ctx, msgno) != 0)
      return -1;
  }

  return 0;
}

/**
 * maildir_sync_message - Sync an email to a Maildir folder
 * @param ctx   Mailbox
 * @param msgno Index number
 * @retval  0 Success
 * @retval -1 Error
 */
static int maildir_sync_message(struct Context *ctx, int msgno)
{
  struct Email *e = ctx->mailbox->hdrs[msgno];
  struct Buffer *newpath = NULL;
  struct Buffer *partpath = NULL;
  struct Buffer *fullpath = NULL;
  struct Buffer *oldpath = NULL;
  char suffix[16];
  int rc = 0;

  if (e->attach_del || e->xlabel_changed ||
      (e->env && (e->env->refs_changed || e->env->irt_changed)))
  {
    /* when doing attachment deletion/rethreading, fall back to the MH case. */
    if (mh_rewrite_message(ctx, msgno) != 0)
      return -1;
  }
  else
  {
    /* we just have to rename the file. */

    char *p = strrchr(e->path, '/');
    if (!p)
    {
      mutt_debug(1, "%s: unable to find subdir!\n", e->path);
      return -1;
    }
    p++;
    newpath = mutt_buffer_pool_get();
    partpath = mutt_buffer_pool_get();
    fullpath = mutt_buffer_pool_get();
    oldpath = mutt_buffer_pool_get();

    mutt_buffer_strcpy(newpath, p);

    /* kill the previous flags */
    p = strchr(newpath->data, ':');
    if (p)
    {
      *p = '\0';
      newpath->dptr = p; /* fix buffer up, just to be safe */
    }

    maildir_gen_flags(suffix, sizeof(suffix), e);

    mutt_buffer_printf(partpath, "%s/%s%s", (e->read || e->old) ? "cur" : "new",
                       mutt_b2s(newpath), suffix);
    mutt_buffer_printf(fullpath, "%s/%s", ctx->mailbox->path, mutt_b2s(partpath));
    mutt_buffer_printf(oldpath, "%s/%s", ctx->mailbox->path, e->path);

    if (mutt_str_strcmp(mutt_b2s(fullpath), mutt_b2s(oldpath)) == 0)
    {
      /* message hasn't really changed */
      goto cleanup;
    }

    /* record that the message is possibly marked as trashed on disk */
    e->trash = e->deleted;

    if (rename(mutt_b2s(oldpath), mutt_b2s(fullpath)) != 0)
    {
      mutt_perror("rename");
      rc = -1;
      goto cleanup;
    }
    mutt_str_replace(&e->path, mutt_b2s(partpath));
  }

cleanup:
  mutt_buffer_pool_release(&newpath);
  mutt_buffer_pool_release(&partpath);
  mutt_buffer_pool_release(&fullpath);
  mutt_buffer_pool_release(&oldpath);

  return rc;
}

/**
 * maildir_canon_filename - Generate the canonical filename for a Maildir folder
 * @param dest   Buffer for the result
 * @param src    Buffer containing source filename
 */
static void maildir_canon_filename(struct Buffer *dest, const char *src)
{
  char *t = strrchr(src, '/');
  if (t)
    src = t + 1;

  mutt_buffer_strcpy(dest, src);
  char *u = strrchr(dest->data, ':');
  if (u)
  {
    *u = '\0';
    dest->dptr = u;
  }
}

/**
 * maildir_update_tables - Update the Header tables
 * @param ctx        Mailbox
 * @param index_hint Current email in index
 */
static void maildir_update_tables(struct Context *ctx, int *index_hint)
{
  if (Sort != SORT_ORDER)
  {
    const short old_sort = Sort;
    Sort = SORT_ORDER;
    mutt_sort_headers(ctx, true);
    Sort = old_sort;
  }

  const int old_count = ctx->mailbox->msg_count;
  for (int i = 0, j = 0; i < old_count; i++)
  {
    if (ctx->mailbox->hdrs[i]->active && index_hint && *index_hint == i)
      *index_hint = j;

    if (ctx->mailbox->hdrs[i]->active)
      ctx->mailbox->hdrs[i]->index = j++;
  }

  mx_update_tables(ctx, false);
  mutt_clear_threads(ctx);
}

/**
 * md_open_find_message - Find a message in a maildir folder
 * @param folder    Base folder
 * @param unique    Unique part of filename
 * @param subfolder Subfolder to search, e.g. 'cur'
 * @param newname   File's new name
 * @retval ptr File handle
 *
 * These functions try to find a message in a maildir folder when it
 * has moved under our feet.  Note that this code is rather expensive, but
 * then again, it's called rarely.
 */
static FILE *md_open_find_message(const char *folder, const char *unique,
                                  const char *subfolder, char **newname)
{
  struct Buffer *dir = mutt_buffer_pool_get();
  struct Buffer *tunique = mutt_buffer_pool_get();
  struct Buffer *fname = mutt_buffer_pool_get();

  struct dirent *de = NULL;

  FILE *fp = NULL;
  int oe = ENOENT;

  mutt_buffer_printf(dir, "%s/%s", folder, subfolder);

  DIR *dp = opendir(mutt_b2s(dir));
  if (!dp)
  {
    errno = ENOENT;
    goto cleanup;
  }

  while ((de = readdir(dp)))
  {
    maildir_canon_filename(tunique, de->d_name);

    if (mutt_str_strcmp(mutt_b2s(tunique), unique) == 0)
    {
      mutt_buffer_printf(fname, "%s/%s/%s", folder, subfolder, de->d_name);
      fp = fopen(mutt_b2s(fname), "r");
      oe = errno;
      break;
    }
  }

  closedir(dp);

  if (newname && fp)
    *newname = mutt_str_strdup(mutt_b2s(fname));

  errno = oe;

cleanup:
  mutt_buffer_pool_release(&dir);
  mutt_buffer_pool_release(&tunique);
  mutt_buffer_pool_release(&fname);

  return fp;
}

/**
 * mh_mailbox - Check for new mail for a mh mailbox
 * @param mailbox     Mailbox to check
 * @param check_stats Also count total, new, and flagged messages
 * @retval true if the mailbox has new mail
 */
bool mh_mailbox(struct Mailbox *mailbox, bool check_stats)
{
  struct MhSequences mhs = { 0 };
  bool check_new = true;
  bool rc = false;
  DIR *dirp = NULL;
  struct dirent *de = NULL;

  /* when $mail_check_recent is set and the .mh_sequences file hasn't changed
   * since the last mailbox visit, there is no "new mail" */
  if (MailCheckRecent && mh_sequences_changed(mailbox) <= 0)
  {
    rc = false;
    check_new = false;
  }

  if (!(check_new || check_stats))
    return rc;

  if (mh_read_sequences(&mhs, mailbox->path) < 0)
    return false;

  if (check_stats)
  {
    mailbox->msg_count = 0;
    mailbox->msg_unread = 0;
    mailbox->msg_flagged = 0;
  }

  for (int i = mhs.max; i > 0; i--)
  {
    if (check_stats && (mhs_check(&mhs, i) & MH_SEQ_FLAGGED))
      mailbox->msg_flagged++;
    if (mhs_check(&mhs, i) & MH_SEQ_UNSEEN)
    {
      if (check_stats)
        mailbox->msg_unread++;
      if (check_new)
      {
        /* if the first unseen message we encounter was in the mailbox during the
           last visit, don't notify about it */
        if (!MailCheckRecent || mh_already_notified(mailbox, i) == 0)
        {
          mailbox->has_new = true;
          rc = true;
        }
        /* Because we are traversing from high to low, we can stop
         * checking for new mail after the first unseen message.
         * Whether it resulted in "new mail" or not. */
        check_new = false;
        if (!check_stats)
          break;
      }
    }
  }
  mhs_free_sequences(&mhs);

  if (check_stats)
  {
    dirp = opendir(mailbox->path);
    if (dirp)
    {
      while ((de = readdir(dirp)))
      {
        if (*de->d_name == '.')
          continue;
        if (mh_valid_message(de->d_name))
          mailbox->msg_count++;
      }
      closedir(dirp);
    }
  }

  return rc;
}

/**
 * maildir_parse_flags - Parse Maildir file flags
 * @param e    Email
 * @param path Path to email file
 */
void maildir_parse_flags(struct Email *e, const char *path)
{
  char *q = NULL;

  e->flagged = false;
  e->read = false;
  e->replied = false;

  char *p = strrchr(path, ':');
  if (p && (mutt_str_strncmp(p + 1, "2,", 2) == 0))
  {
    p += 3;

    mutt_str_replace(&e->maildir_flags, p);
    q = e->maildir_flags;

    while (*p)
    {
      switch (*p)
      {
        case 'F':
          e->flagged = true;
          break;

        case 'R': /* replied */
          e->replied = true;
          break;

        case 'S': /* seen */
          e->read = true;
          break;

        case 'T': /* trashed */
          if (!e->flagged || !FlagSafe)
          {
            e->trash = true;
            e->deleted = true;
          }
          break;

        default:
          *q++ = *p;
          break;
      }
      p++;
    }
  }

  if (q == e->maildir_flags)
    FREE(&e->maildir_flags);
  else if (q)
    *q = '\0';
}

/**
 * maildir_parse_stream - Parse a Maildir message
 * @param magic  Mailbox type, e.g. #MUTT_MAILDIR
 * @param f      Mesage file handle
 * @param fname  Message filename
 * @param is_old true, if the email is old (read)
 * @param e      Email Header to populate (OPTIONAL)
 * @retval ptr Populated email Header
 *
 * Actually parse a maildir message.  This may also be used to fill
 * out a fake header structure generated by lazy maildir parsing.
 */
struct Email *maildir_parse_stream(enum MailboxType magic, FILE *f,
                                   const char *fname, bool is_old, struct Email *e)
{
  struct stat st;

  if (!e)
    e = mutt_email_new();
  e->env = mutt_rfc822_read_header(f, e, false, false);

  fstat(fileno(f), &st);

  if (!e->received)
    e->received = e->date_sent;

  /* always update the length since we have fresh information available. */
  e->content->length = st.st_size - e->content->offset;

  e->index = -1;

  if (magic == MUTT_MAILDIR)
  {
    /* maildir stores its flags in the filename, so ignore the
     * flags in the header of the message
     */

    e->old = is_old;
    maildir_parse_flags(e, fname);
  }
  return e;
}

/**
 * maildir_parse_message - Actually parse a maildir message
 * @param magic  Mailbox type, e.g. #MUTT_MAILDIR
 * @param fname  Message filename
 * @param is_old true, if the email is old (read)
 * @param e      Email Header to populate (OPTIONAL)
 * @retval ptr Populated email Header
 *
 * This may also be used to fill out a fake header structure generated by lazy
 * maildir parsing.
 */
struct Email *maildir_parse_message(enum MailboxType magic, const char *fname,
                                    bool is_old, struct Email *e)
{
  FILE *f = fopen(fname, "r");
  if (!f)
    return NULL;

  e = maildir_parse_stream(magic, f, fname, is_old, e);
  mutt_file_fclose(&f);
  return e;
}

/**
 * maildir_flags - Generate the Maildir flags for an email
 * @param dest    Buffer for the result
 * @param destlen Length of buffer
 * @param e     Email
 */
void maildir_gen_flags(char *dest, size_t destlen, struct Email *e)
{
  *dest = '\0';

  /* The maildir specification requires that all files in the cur
   * subdirectory have the :unique string appended, regardless of whether
   * or not there are any flags.  If .old is set, we know that this message
   * will end up in the cur directory, so we include it in the following
   * test even though there is no associated flag.
   */

  if (e && (e->flagged || e->replied || e->read || e->deleted || e->old || e->maildir_flags))
  {
    char tmp[LONG_STRING];
    snprintf(tmp, sizeof(tmp), "%s%s%s%s%s", e->flagged ? "F" : "", e->replied ? "R" : "",
             e->read ? "S" : "", e->deleted ? "T" : "", NONULL(e->maildir_flags));
    if (e->maildir_flags)
      qsort(tmp, strlen(tmp), 1, ch_compare);
    snprintf(dest, destlen, ":2,%s", tmp);
  }
}

/**
 * mh_sync_mailbox_message - Save changes to the mailbox
 * @param ctx   Mailbox
 * @param msgno Index number
 * @param hc    Header cache handle
 * @retval  0 Success
 * @retval -1 Error
 */
#ifdef USE_HCACHE
int mh_sync_mailbox_message(struct Context *ctx, int msgno, header_cache_t *hc)
#else
int mh_sync_mailbox_message(struct Context *ctx, int msgno)
#endif
{
  struct Email *e = ctx->mailbox->hdrs[msgno];

  if (e->deleted && (ctx->mailbox->magic != MUTT_MAILDIR || !MaildirTrash))
  {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", ctx->mailbox->path, e->path);
    if (ctx->mailbox->magic == MUTT_MAILDIR || (MhPurge && ctx->mailbox->magic == MUTT_MH))
    {
#ifdef USE_HCACHE
      if (hc)
      {
        const char *key = NULL;
        size_t keylen;
        if (ctx->mailbox->magic == MUTT_MH)
        {
          key = e->path;
          keylen = strlen(key);
        }
        else
        {
          key = e->path + 3;
          keylen = maildir_hcache_keylen(key);
        }
        mutt_hcache_delete(hc, key, keylen);
      }
#endif
      unlink(path);
    }
    else if (ctx->mailbox->magic == MUTT_MH)
    {
      /* MH just moves files out of the way when you delete them */
      if (*e->path != ',')
      {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/,%s", ctx->mailbox->path, e->path);
        unlink(tmp);
        rename(path, tmp);
      }
    }
  }
  else if (e->changed || e->attach_del || e->xlabel_changed ||
           (ctx->mailbox->magic == MUTT_MAILDIR && (MaildirTrash || e->trash) &&
            (e->deleted != e->trash)))
  {
    if (ctx->mailbox->magic == MUTT_MAILDIR)
    {
      if (maildir_sync_message(ctx, msgno) == -1)
        return -1;
    }
    else
    {
      if (mh_sync_message(ctx, msgno) == -1)
        return -1;
    }
  }

#ifdef USE_HCACHE
  if (hc && e->changed)
  {
    const char *key = NULL;
    size_t keylen;
    if (ctx->mailbox->magic == MUTT_MH)
    {
      key = e->path;
      keylen = strlen(key);
    }
    else
    {
      key = e->path + 3;
      keylen = maildir_hcache_keylen(key);
    }
    mutt_hcache_store(hc, key, keylen, e, 0);
  }
#endif

  return 0;
}

/**
 * maildir_update_flags - Update the mailbox flags
 * @param ctx Mailbox
 * @param o   Old email Header
 * @param n   New email Header
 * @retval true  If the flags changed
 * @retval false Otherwise
 */
bool maildir_update_flags(struct Context *ctx, struct Email *o, struct Email *n)
{
  /* save the global state here so we can reset it at the
   * end of list block if required.
   */
  bool context_changed = ctx->mailbox->changed;

  /* user didn't modify this message.  alter the flags to match the
   * current state on disk.  This may not actually do
   * anything. mutt_set_flag() will just ignore the call if the status
   * bits are already properly set, but it is still faster not to pass
   * through it */
  if (o->flagged != n->flagged)
    mutt_set_flag(ctx, o, MUTT_FLAG, n->flagged);
  if (o->replied != n->replied)
    mutt_set_flag(ctx, o, MUTT_REPLIED, n->replied);
  if (o->read != n->read)
    mutt_set_flag(ctx, o, MUTT_READ, n->read);
  if (o->old != n->old)
    mutt_set_flag(ctx, o, MUTT_OLD, n->old);

  /* mutt_set_flag() will set this, but we don't need to
   * sync the changes we made because we just updated the
   * context to match the current on-disk state of the
   * message.
   */
  bool header_changed = o->changed;
  o->changed = false;

  /* if the mailbox was not modified before we made these
   * changes, unset the changed flag since nothing needs to
   * be synchronized.
   */
  if (!context_changed)
    ctx->mailbox->changed = false;

  return header_changed;
}

/**
 * maildir_open_find_message - Find a new
 * @param folder  Maildir path
 * @param msg     Email path
 * @param newname New name, if it has moved
 * @retval ptr File handle
 */
FILE *maildir_open_find_message(const char *folder, const char *msg, char **newname)
{
  static unsigned int new_hits = 0, cur_hits = 0; /* simple dynamic optimization */

  struct Buffer *unique = mutt_buffer_pool_get();
  maildir_canon_filename(unique, msg);

  FILE *fp = md_open_find_message(folder, mutt_b2s(unique),
                                  new_hits > cur_hits ? "new" : "cur", newname);
  if (fp || (errno != ENOENT))
  {
    if (new_hits < UINT_MAX && cur_hits < UINT_MAX)
    {
      new_hits += (new_hits > cur_hits ? 1 : 0);
      cur_hits += (new_hits > cur_hits ? 0 : 1);
    }

    goto cleanup;
  }
  fp = md_open_find_message(folder, mutt_b2s(unique),
                            new_hits > cur_hits ? "cur" : "new", newname);
  if (fp || (errno != ENOENT))
  {
    if (new_hits < UINT_MAX && cur_hits < UINT_MAX)
    {
      new_hits += (new_hits > cur_hits ? 0 : 1);
      cur_hits += (new_hits > cur_hits ? 1 : 0);
    }

    goto cleanup;
  }

  fp = NULL;

cleanup:
  mutt_buffer_pool_release(&unique);

  return fp;
}

/**
 * maildir_check_empty - Is the mailbox empty
 * @param path Mailbox to check
 * @retval 1 Mailbox is empty
 * @retval 0 Mailbox contains mail
 * @retval -1 Error
 */
int maildir_check_empty(const char *path)
{
  DIR *dp = NULL;
  struct dirent *de = NULL;
  int r = 1; /* assume empty until we find a message */
  char realpath[PATH_MAX];
  int iter = 0;

  /* Strategy here is to look for any file not beginning with a period */

  do
  {
    /* we do "cur" on the first iteration since it's more likely that we'll
     * find old messages without having to scan both subdirs
     */
    snprintf(realpath, sizeof(realpath), "%s/%s", path, iter == 0 ? "cur" : "new");
    dp = opendir(realpath);
    if (!dp)
      return -1;
    while ((de = readdir(dp)))
    {
      if (*de->d_name != '.')
      {
        r = 0;
        break;
      }
    }
    closedir(dp);
    iter++;
  } while (r && iter < 2);

  return r;
}

/**
 * mh_check_empty - Is mailbox empty
 * @param path Mailbox to check
 * @retval 1 Mailbox is empty
 * @retval 0 Mailbox contains mail
 * @retval -1 Error
 */
int mh_check_empty(const char *path)
{
  struct dirent *de = NULL;
  int r = 1; /* assume empty until we find a message */

  DIR *dp = opendir(path);
  if (!dp)
    return -1;
  while ((de = readdir(dp)))
  {
    if (mh_valid_message(de->d_name))
    {
      r = 0;
      break;
    }
  }
  closedir(dp);

  return r;
}

/**
 * maildir_mbox_open - Implements MxOps::mbox_open()
 */
static int maildir_mbox_open(struct Context *ctx)
{
  return maildir_read_dir(ctx);
}

/**
 * maildir_mbox_open_append - Implements MxOps::mbox_open_append()
 */
static int maildir_mbox_open_append(struct Context *ctx, int flags)
{
  if (!(flags & MUTT_APPENDNEW))
  {
    return 0;
  }

  if (mkdir(ctx->mailbox->path, S_IRWXU))
  {
    mutt_perror(ctx->mailbox->path);
    return -1;
  }

  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s/cur", ctx->mailbox->path);
  if (mkdir(tmp, S_IRWXU))
  {
    mutt_perror(tmp);
    rmdir(ctx->mailbox->path);
    return -1;
  }

  snprintf(tmp, sizeof(tmp), "%s/new", ctx->mailbox->path);
  if (mkdir(tmp, S_IRWXU))
  {
    mutt_perror(tmp);
    snprintf(tmp, sizeof(tmp), "%s/cur", ctx->mailbox->path);
    rmdir(tmp);
    rmdir(ctx->mailbox->path);
    return -1;
  }

  snprintf(tmp, sizeof(tmp), "%s/tmp", ctx->mailbox->path);
  if (mkdir(tmp, S_IRWXU))
  {
    mutt_perror(tmp);
    snprintf(tmp, sizeof(tmp), "%s/cur", ctx->mailbox->path);
    rmdir(tmp);
    snprintf(tmp, sizeof(tmp), "%s/new", ctx->mailbox->path);
    rmdir(tmp);
    rmdir(ctx->mailbox->path);
    return -1;
  }

  return 0;
}

/**
 * maildir_mbox_check - Implements MxOps::mbox_check()
 *
 * This function handles arrival of new mail and reopening of maildir folders.
 * The basic idea here is we check to see if either the new or cur
 * subdirectories have changed, and if so, we scan them for the list of files.
 * We check for newly added messages, and then merge the flags messages we
 * already knew about.  We don't treat either subdirectory differently, as mail
 * could be copied directly into the cur directory from another agent.
 */
static int maildir_mbox_check(struct Context *ctx, int *index_hint)
{
  struct stat st_new;         /* status of the "new" subdirectory */
  struct stat st_cur;         /* status of the "cur" subdirectory */
  int changed = 0;            /* bitmask representing which subdirectories
                                 have changed.  0x1 = new, 0x2 = cur */
  bool occult = false;        /* messages were removed from the mailbox */
  int have_new = 0;           /* messages were added to the mailbox */
  bool flags_changed = false; /* message flags were changed in the mailbox */
  struct Maildir *md = NULL;  /* list of messages in the mailbox */
  struct Maildir **last = NULL, *p = NULL;
  int count = 0;
  struct Hash *fnames = NULL; /* hash table for quickly looking up the base filename
                                 for a maildir message */
  struct MaildirMboxData *mdata = maildir_get_mdata(ctx->mailbox);

  /* XXX seems like this check belongs in mx_mbox_check() rather than here.  */
  if (!CheckNew)
    return 0;

  struct Buffer *buf = mutt_buffer_pool_get();
  mutt_buffer_printf(buf, "%s/new", ctx->mailbox->path);
  if (stat(mutt_b2s(buf), &st_new) == -1)
  {
    mutt_buffer_pool_release(&buf);
    return -1;
  }

  mutt_buffer_printf(buf, "%s/cur", ctx->mailbox->path);
  if (stat(mutt_b2s(buf), &st_cur) == -1)
  {
    mutt_buffer_pool_release(&buf);
    return -1;
  }

  /* determine which subdirectories need to be scanned */
  if (mutt_stat_timespec_compare(&st_new, MUTT_STAT_MTIME, &ctx->mailbox->mtime) > 0)
    changed = 1;
  if (mutt_stat_timespec_compare(&st_cur, MUTT_STAT_MTIME, &mdata->mtime_cur) > 0)
    changed |= 2;

  if (!changed)
  {
    mutt_buffer_pool_release(&buf);
    return 0; /* nothing to do */
  }

  /* Update the modification times on the mailbox.
   *
   * The monitor code notices changes in the open mailbox too quickly.
   * In practice, this sometimes leads to all the new messages not being
   * noticed during the SAME group of mtime stat updates.  To work around
   * the problem, don't update the stat times for a monitor caused check. */
#ifdef USE_INOTIFY
  if (MonitorContextChanged)
    MonitorContextChanged = 0;
  else
#endif
  {
    mutt_get_stat_timespec(&mdata->mtime_cur, &st_cur, MUTT_STAT_MTIME);
    mutt_get_stat_timespec(&ctx->mailbox->mtime, &st_new, MUTT_STAT_MTIME);
  }

  /* do a fast scan of just the filenames in
   * the subdirectories that have changed.
   */
  md = NULL;
  last = &md;
  if (changed & 1)
    maildir_parse_dir(ctx->mailbox, &last, "new", &count, NULL);
  if (changed & 2)
    maildir_parse_dir(ctx->mailbox, &last, "cur", &count, NULL);

  /* we create a hash table keyed off the canonical (sans flags) filename
   * of each message we scanned.  This is used in the loop over the
   * existing messages below to do some correlation.
   */
  fnames = mutt_hash_create(count, 0);

  for (p = md; p; p = p->next)
  {
    maildir_canon_filename(buf, p->email->path);
    p->canon_fname = mutt_str_strdup(mutt_b2s(buf));
    mutt_hash_insert(fnames, p->canon_fname, p);
  }

  /* check for modifications and adjust flags */
  for (int i = 0; i < ctx->mailbox->msg_count; i++)
  {
    ctx->mailbox->hdrs[i]->active = false;
    maildir_canon_filename(buf, ctx->mailbox->hdrs[i]->path);
    p = mutt_hash_find(fnames, mutt_b2s(buf));
    if (p && p->email)
    {
      /* message already exists, merge flags */
      ctx->mailbox->hdrs[i]->active = true;

      /* check to see if the message has moved to a different
       * subdirectory.  If so, update the associated filename.
       */
      if (mutt_str_strcmp(ctx->mailbox->hdrs[i]->path, p->email->path) != 0)
        mutt_str_replace(&ctx->mailbox->hdrs[i]->path, p->email->path);

      /* if the user hasn't modified the flags on this message, update
       * the flags we just detected.
       */
      if (!ctx->mailbox->hdrs[i]->changed)
        if (maildir_update_flags(ctx, ctx->mailbox->hdrs[i], p->email))
          flags_changed = true;

      if (ctx->mailbox->hdrs[i]->deleted == ctx->mailbox->hdrs[i]->trash)
      {
        if (ctx->mailbox->hdrs[i]->deleted != p->email->deleted)
        {
          ctx->mailbox->hdrs[i]->deleted = p->email->deleted;
          flags_changed = true;
        }
      }
      ctx->mailbox->hdrs[i]->trash = p->email->trash;

      /* this is a duplicate of an existing header, so remove it */
      mutt_email_free(&p->email);
    }
    /* This message was not in the list of messages we just scanned.
     * Check to see if we have enough information to know if the
     * message has disappeared out from underneath us.
     */
    else if (((changed & 1) && (strncmp(ctx->mailbox->hdrs[i]->path, "new/", 4) == 0)) ||
             ((changed & 2) && (strncmp(ctx->mailbox->hdrs[i]->path, "cur/", 4) == 0)))
    {
      /* This message disappeared, so we need to simulate a "reopen"
       * event.  We know it disappeared because we just scanned the
       * subdirectory it used to reside in.
       */
      occult = true;
    }
    else
    {
      /* This message resides in a subdirectory which was not
       * modified, so we assume that it is still present and
       * unchanged.
       */
      ctx->mailbox->hdrs[i]->active = true;
    }
  }

  /* destroy the file name hash */
  mutt_hash_destroy(&fnames);

  /* If we didn't just get new mail, update the tables. */
  if (occult)
    maildir_update_tables(ctx, index_hint);

  /* do any delayed parsing we need to do. */
  maildir_delayed_parsing(ctx->mailbox, &md, NULL);

  /* Incorporate new messages */
  have_new = maildir_move_to_context(ctx, &md);

  mutt_buffer_pool_release(&buf);

  if (occult)
    return MUTT_REOPENED;
  if (have_new)
    return MUTT_NEW_MAIL;
  if (flags_changed)
    return MUTT_FLAGS;
  return 0;
}

/**
 * maildir_msg_open - Implements MxOps::msg_open()
 */
static int maildir_msg_open(struct Context *ctx, struct Message *msg, int msgno)
{
  return maildir_mh_open_message(ctx->mailbox, msg, msgno, 1);
}

/**
 * maildir_msg_open_new - Implements MxOps::msg_open_new()
 *
 * Open a new (temporary) message in a maildir folder.
 *
 * @note This uses _almost_ the maildir file name format,
 * but with a {cur,new} prefix.
 */
static int maildir_msg_open_new(struct Context *ctx, struct Message *msg, struct Email *e)
{
  int fd;
  char path[PATH_MAX];
  char suffix[16];
  char subdir[16];

  if (e)
  {
    bool deleted = e->deleted;
    e->deleted = false;

    maildir_gen_flags(suffix, sizeof(suffix), e);

    e->deleted = deleted;
  }
  else
    *suffix = '\0';

  if (e && (e->read || e->old))
    mutt_str_strfcpy(subdir, "cur", sizeof(subdir));
  else
    mutt_str_strfcpy(subdir, "new", sizeof(subdir));

  mode_t omask = umask(mh_umask(ctx->mailbox));
  while (true)
  {
    snprintf(path, sizeof(path), "%s/tmp/%s.%lld.R%" PRIu64 ".%s%s",
             ctx->mailbox->path, subdir, (long long) time(NULL), mutt_rand64(),
             NONULL(ShortHostname), suffix);

    mutt_debug(2, "Trying %s.\n", path);

    fd = open(path, O_WRONLY | O_EXCL | O_CREAT, 0666);
    if (fd == -1)
    {
      if (errno != EEXIST)
      {
        umask(omask);
        mutt_perror(path);
        return -1;
      }
    }
    else
    {
      mutt_debug(2, "Success.\n");
      msg->path = mutt_str_strdup(path);
      break;
    }
  }
  umask(omask);

  msg->fp = fdopen(fd, "w");
  if (!msg->fp)
  {
    FREE(&msg->path);
    close(fd);
    unlink(path);
    return -1;
  }

  return 0;
}

/**
 * maildir_msg_commit - Implements MxOps::msg_commit()
 */
static int maildir_msg_commit(struct Context *ctx, struct Message *msg)
{
  return md_commit_message(ctx->mailbox, msg, NULL);
}

/**
 * maildir_path_probe - Is this a Maildir mailbox? - Implements MxOps::path_probe()
 */
int maildir_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (!st || !S_ISDIR(st->st_mode))
    return MUTT_UNKNOWN;

  char cur[PATH_MAX];
  snprintf(cur, sizeof(cur), "%s/cur", path);

  struct stat stc;
  if ((stat(cur, &stc) == 0) && S_ISDIR(stc.st_mode))
    return MUTT_MAILDIR;

  return MUTT_UNKNOWN;
}

/**
 * maildir_path_canon - Canonicalise a mailbox path - Implements MxOps::path_canon()
 */
int maildir_path_canon(char *buf, size_t buflen, const char *folder)
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
 * maildir_path_pretty - Implements MxOps::path_pretty()
 */
int maildir_path_pretty(char *buf, size_t buflen, const char *folder)
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
 * maildir_path_parent - Implements MxOps::path_parent()
 */
int maildir_path_parent(char *buf, size_t buflen)
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

/**
 * mh_mbox_open - Implements MxOps::mbox_open()
 */
static int mh_mbox_open(struct Context *ctx)
{
  return mh_read_dir(ctx, NULL);
}

/**
 * mh_mbox_open_append - Implements MxOps::mbox_open_append()
 */
static int mh_mbox_open_append(struct Context *ctx, int flags)
{
  if (!(flags & MUTT_APPENDNEW))
  {
    return 0;
  }

  if (mkdir(ctx->mailbox->path, S_IRWXU))
  {
    mutt_perror(ctx->mailbox->path);
    return -1;
  }

  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s/.mh_sequences", ctx->mailbox->path);
  const int i = creat(tmp, S_IRWXU);
  if (i == -1)
  {
    mutt_perror(tmp);
    rmdir(ctx->mailbox->path);
    return -1;
  }
  close(i);

  return 0;
}

/**
 * mh_mbox_check - Implements MxOps::mbox_check()
 *
 * This function handles arrival of new mail and reopening of mh/maildir
 * folders. Things are getting rather complex because we don't have a
 * well-defined "mailbox order", so the tricks from mbox.c and mx.c won't work
 * here.
 *
 * Don't change this code unless you _really_ understand what happens.
 */
static int mh_mbox_check(struct Context *ctx, int *index_hint)
{
  char buf[PATH_MAX];
  struct stat st, st_cur;
  bool modified = false, have_new = false, occult = false, flags_changed = false;
  struct Maildir *md = NULL, *p = NULL;
  struct Maildir **last = NULL;
  struct MhSequences mhs = { 0 };
  int count = 0;
  struct Hash *fnames = NULL;
  struct MaildirMboxData *mdata = maildir_get_mdata(ctx->mailbox);

  if (!CheckNew)
    return 0;

  mutt_str_strfcpy(buf, ctx->mailbox->path, sizeof(buf));
  if (stat(buf, &st) == -1)
    return -1;

  /* create .mh_sequences when there isn't one. */
  snprintf(buf, sizeof(buf), "%s/.mh_sequences", ctx->mailbox->path);
  int i = stat(buf, &st_cur);
  if ((i == -1) && (errno == ENOENT))
  {
    char *tmp = NULL;
    FILE *fp = NULL;

    if (mh_mkstemp(ctx->mailbox, &fp, &tmp) == 0)
    {
      mutt_file_fclose(&fp);
      if (mutt_file_safe_rename(tmp, buf) == -1)
        unlink(tmp);
      FREE(&tmp);
    }
  }

  if (i == -1 && stat(buf, &st_cur) == -1)
    modified = true;

  if ((mutt_stat_timespec_compare(&st, MUTT_STAT_MTIME, &ctx->mailbox->mtime) > 0) ||
      (mutt_stat_timespec_compare(&st_cur, MUTT_STAT_MTIME, &mdata->mtime_cur) > 0))
  {
    modified = true;
  }

  if (!modified)
    return 0;

    /* Update the modification times on the mailbox.
     *
     * The monitor code notices changes in the open mailbox too quickly.
     * In practice, this sometimes leads to all the new messages not being
     * noticed during the SAME group of mtime stat updates.  To work around
     * the problem, don't update the stat times for a monitor caused check. */
#ifdef USE_INOTIFY
  if (MonitorContextChanged)
    MonitorContextChanged = 0;
  else
#endif
  {
    mutt_get_stat_timespec(&mdata->mtime_cur, &st_cur, MUTT_STAT_MTIME);
    mutt_get_stat_timespec(&ctx->mailbox->mtime, &st, MUTT_STAT_MTIME);
  }

  md = NULL;
  last = &md;

  maildir_parse_dir(ctx->mailbox, &last, NULL, &count, NULL);
  maildir_delayed_parsing(ctx->mailbox, &md, NULL);

  if (mh_read_sequences(&mhs, ctx->mailbox->path) < 0)
    return -1;
  mh_update_maildir(md, &mhs);
  mhs_free_sequences(&mhs);

  /* check for modifications and adjust flags */
  fnames = mutt_hash_create(count, 0);

  for (p = md; p; p = p->next)
  {
    /* the hash key must survive past the header, which is freed below. */
    p->canon_fname = mutt_str_strdup(p->email->path);
    mutt_hash_insert(fnames, p->canon_fname, p);
  }

  for (i = 0; i < ctx->mailbox->msg_count; i++)
  {
    ctx->mailbox->hdrs[i]->active = false;

    p = mutt_hash_find(fnames, ctx->mailbox->hdrs[i]->path);
    if (p && p->email && mutt_email_cmp_strict(ctx->mailbox->hdrs[i], p->email))
    {
      ctx->mailbox->hdrs[i]->active = true;
      /* found the right message */
      if (!ctx->mailbox->hdrs[i]->changed)
        if (maildir_update_flags(ctx, ctx->mailbox->hdrs[i], p->email))
          flags_changed = true;

      mutt_email_free(&p->email);
    }
    else /* message has disappeared */
      occult = true;
  }

  /* destroy the file name hash */

  mutt_hash_destroy(&fnames);

  /* If we didn't just get new mail, update the tables. */
  if (occult)
    maildir_update_tables(ctx, index_hint);

  /* Incorporate new messages */
  have_new = maildir_move_to_context(ctx, &md);

  if (occult)
    return MUTT_REOPENED;
  if (have_new)
    return MUTT_NEW_MAIL;
  if (flags_changed)
    return MUTT_FLAGS;
  return 0;
}

/**
 * mh_mbox_sync - Implements MxOps::mbox_sync()
 */
static int mh_mbox_sync(struct Context *ctx, int *index_hint)
{
  int i, j;
#ifdef USE_HCACHE
  header_cache_t *hc = NULL;
#endif
  char msgbuf[PATH_MAX + 64];
  struct Progress progress;

  if (ctx->mailbox->magic == MUTT_MH)
    i = mh_mbox_check(ctx, index_hint);
  else
    i = maildir_mbox_check(ctx, index_hint);

  if (i != 0)
    return i;

#ifdef USE_HCACHE
  if (ctx->mailbox->magic == MUTT_MAILDIR || ctx->mailbox->magic == MUTT_MH)
    hc = mutt_hcache_open(HeaderCache, ctx->mailbox->path, NULL);
#endif

  if (!ctx->mailbox->quiet)
  {
    snprintf(msgbuf, sizeof(msgbuf), _("Writing %s..."), ctx->mailbox->path);
    mutt_progress_init(&progress, msgbuf, MUTT_PROGRESS_MSG, WriteInc,
                       ctx->mailbox->msg_count);
  }

  for (i = 0; i < ctx->mailbox->msg_count; i++)
  {
    if (!ctx->mailbox->quiet)
      mutt_progress_update(&progress, i, -1);

#ifdef USE_HCACHE
    if (mh_sync_mailbox_message(ctx, i, hc) == -1)
      goto err;
#else
    if (mh_sync_mailbox_message(ctx, i) == -1)
      goto err;
#endif
  }

#ifdef USE_HCACHE
  if (ctx->mailbox->magic == MUTT_MAILDIR || ctx->mailbox->magic == MUTT_MH)
    mutt_hcache_close(hc);
#endif

  if (ctx->mailbox->magic == MUTT_MH)
    mh_update_sequences(ctx->mailbox);

  /* XXX race condition? */

  maildir_update_mtime(ctx->mailbox);

  /* adjust indices */

  if (ctx->deleted)
  {
    for (i = 0, j = 0; i < ctx->mailbox->msg_count; i++)
    {
      if (!ctx->mailbox->hdrs[i]->deleted || (ctx->mailbox->magic == MUTT_MAILDIR && MaildirTrash))
        ctx->mailbox->hdrs[i]->index = j++;
    }
  }

  return 0;

err:
#ifdef USE_HCACHE
  if (ctx->mailbox->magic == MUTT_MAILDIR || ctx->mailbox->magic == MUTT_MH)
    mutt_hcache_close(hc);
#endif
  return -1;
}

/**
 * mh_mbox_close - Implements MxOps::mbox_close()
 * @retval 0 Always
 */
static int mh_mbox_close(struct Context *ctx)
{
  FREE(&ctx->mailbox->data);

  return 0;
}

/**
 * mh_msg_open - Implements MxOps::msg_open()
 */
static int mh_msg_open(struct Context *ctx, struct Message *msg, int msgno)
{
  return maildir_mh_open_message(ctx->mailbox, msg, msgno, 0);
}

/**
 * mh_msg_open_new - Implements MxOps::msg_open_new()
 *
 * Open a new (temporary) message in an MH folder.
 */
static int mh_msg_open_new(struct Context *ctx, struct Message *msg, struct Email *e)
{
  return mh_mkstemp(ctx->mailbox, &msg->fp, &msg->path);
}

/**
 * mh_msg_commit - Implements MxOps::msg_commit()
 */
static int mh_msg_commit(struct Context *ctx, struct Message *msg)
{
  return mh_commit_msg(ctx->mailbox, msg, NULL, true);
}

/**
 * mh_msg_close - Implements MxOps::msg_close()
 *
 * @note May also return EOF Failure, see errno
 */
static int mh_msg_close(struct Context *ctx, struct Message *msg)
{
  return mutt_file_fclose(&msg->fp);
}

/**
 * mh_path_probe - Is this an mh mailbox? - Implements MxOps::path_probe()
 */
int mh_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (!st || !S_ISDIR(st->st_mode))
    return MUTT_UNKNOWN;

  char tmp[PATH_MAX];

  snprintf(tmp, sizeof(tmp), "%s/.mh_sequences", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  snprintf(tmp, sizeof(tmp), "%s/.xmhcache", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  snprintf(tmp, sizeof(tmp), "%s/.mew_cache", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  snprintf(tmp, sizeof(tmp), "%s/.mew-cache", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  snprintf(tmp, sizeof(tmp), "%s/.sylpheed_cache", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  /* ok, this isn't an mh folder, but mh mode can be used to read
   * Usenet news from the spool.  */

  snprintf(tmp, sizeof(tmp), "%s/.overview", path);
  if (access(tmp, F_OK) == 0)
    return MUTT_MH;

  return MUTT_UNKNOWN;
}

// clang-format off
/**
 * struct mx_maildir_ops - Maildir mailbox - Implements ::MxOps
 */
struct MxOps mx_maildir_ops = {
  .magic            = MUTT_MAILDIR,
  .name             = "maildir",
  .mbox_open        = maildir_mbox_open,
  .mbox_open_append = maildir_mbox_open_append,
  .mbox_check       = maildir_mbox_check,
  .mbox_sync        = mh_mbox_sync,
  .mbox_close       = mh_mbox_close,
  .msg_open         = maildir_msg_open,
  .msg_open_new     = maildir_msg_open_new,
  .msg_commit       = maildir_msg_commit,
  .msg_close        = mh_msg_close,
  .msg_padding_size = NULL,
  .tags_edit        = NULL,
  .tags_commit      = NULL,
  .path_probe       = maildir_path_probe,
  .path_canon       = maildir_path_canon,
  .path_pretty      = maildir_path_pretty,
  .path_parent      = maildir_path_parent,
};

/**
 * struct mx_mh_ops - MH mailbox - Implements ::MxOps
 */
struct MxOps mx_mh_ops = {
  .magic            = MUTT_MH,
  .name             = "mh",
  .mbox_open        = mh_mbox_open,
  .mbox_open_append = mh_mbox_open_append,
  .mbox_check       = mh_mbox_check,
  .mbox_sync        = mh_mbox_sync,
  .mbox_close       = mh_mbox_close,
  .msg_open         = mh_msg_open,
  .msg_open_new     = mh_msg_open_new,
  .msg_commit       = mh_msg_commit,
  .msg_close        = mh_msg_close,
  .msg_padding_size = NULL,
  .tags_edit        = NULL,
  .tags_commit      = NULL,
  .path_probe       = mh_path_probe,
  .path_canon       = maildir_path_canon,
  .path_pretty      = maildir_path_pretty,
  .path_parent      = maildir_path_parent,
};
// clang-format on
