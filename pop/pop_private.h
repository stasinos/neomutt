/**
 * @file
 * POP network mailbox
 *
 * @authors
 * Copyright (C) 2000-2003 Vsevolod Volkov <vvv@mutt.org.ua>
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

#ifndef MUTT_POP_POP_PRIVATE_H
#define MUTT_POP_POP_PRIVATE_H

#include <stdbool.h>
#include <time.h>

struct ConnAccount;
struct Context;
struct Mailbox;
struct Progress;

#define POP_PORT 110
#define POP_SSL_PORT 995

/* number of entries in the hash table */
#define POP_CACHE_LEN 10

/* maximal length of the server response (RFC1939) */
#define POP_CMD_RESPONSE 512

/**
 * enum PopStatus - POP server responses
 */
enum PopStatus
{
  POP_NONE = 0,
  POP_CONNECTED,
  POP_DISCONNECTED,
  POP_BYE
};

/**
 * enum PopAuthRes - POP authentication responses
 */
enum PopAuthRes
{
  POP_A_SUCCESS = 0,
  POP_A_SOCKET,
  POP_A_FAILURE,
  POP_A_UNAVAIL
};

/**
 * struct PopCache - POP-specific email cache
 */
struct PopCache
{
  unsigned int index;
  char *path;
};

/**
 * struct PopMboxData - POP data attached to a Mailbox - @extends Mailbox
 */
struct PopMboxData
{
  struct Connection *conn;
  unsigned int status : 2;
  bool capabilities : 1;
  unsigned int use_stls : 2;
  bool cmd_capa : 1;         /**< optional command CAPA */
  bool cmd_stls : 1;         /**< optional command STLS */
  unsigned int cmd_user : 2; /**< optional command USER */
  unsigned int cmd_uidl : 2; /**< optional command UIDL */
  unsigned int cmd_top : 2;  /**< optional command TOP */
  bool resp_codes : 1;       /**< server supports extended response codes */
  bool expire : 1;           /**< expire is greater than 0 */
  bool clear_cache : 1;
  size_t size;
  time_t check_time;
  time_t login_delay; /**< minimal login delay  capability */
  char *auth_list;    /**< list of auth mechanisms */
  char *timestamp;
  struct BodyCache *bcache; /**< body cache */
  char err_msg[POP_CMD_RESPONSE];
  struct PopCache cache[POP_CACHE_LEN];
};

/**
 * struct PopEmailData - POP data attached to an Email - @extends Email
 */
struct PopEmailData
{
  const char *uid;
};

/**
 * struct PopAuth - POP authentication multiplexor
 */
struct PopAuth
{
  /* do authentication, using named method or any available if method is NULL */
  enum PopAuthRes (*authenticate)(struct PopMboxData *, const char *);
  /* name of authentication method supported, NULL means variable. If this
   * is not null, authenticate may ignore the second parameter. */
  const char *method;
};

/* pop_auth.c */
int pop_authenticate(struct PopMboxData *mdata);
void pop_apop_timestamp(struct PopMboxData *mdata, char *buf);

/* pop_lib.c */
#define pop_query(A, B, C) pop_query_d(A, B, C, NULL)
int pop_parse_path(const char *path, struct ConnAccount *acct);
int pop_connect(struct PopMboxData *mdata);
int pop_open_connection(struct PopMboxData *mdata);
int pop_query_d(struct PopMboxData *mdata, char *buf, size_t buflen, char *msg);
int pop_fetch_data(struct PopMboxData *mdata, const char *query, struct Progress *progressbar,
                   int (*func)(char *, void *), void *data);
int pop_reconnect(struct Mailbox *mailbox);
void pop_logout(struct Mailbox *mailbox);
struct PopMboxData *pop_get_mdata(struct Mailbox *m);

#endif /* MUTT_POP_POP_PRIVATE_H */
