/*
 * Copyright (c) 2016 Angel Gonzalez <aglezabad@gmail.com>
 * Based on pgsql.h of Kore framework:
 * Copyright (c) 2014 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _H_KORE_MYSQL
#define _H_KORE_MYSQL

#include <my_config.h>
#include <mysql.h>

#define KORE_MYSQL_FORMAT_TEXT		0
#define KORE_MYSQL_FORMAT_BINARY	1

#define KORE_MYSQL_SYNC			0x0001
#define KORE_MYSQL_ASYNC		0x0002

#if defined(__cplusplus)
extern "C" {
#endif

struct mysql_conn {
    MYSQL *mysql;

	struct mysql_job		*job;
	TAILQ_ENTRY(mysql_conn)		list;
};

struct mysql_db {
    const char *host;
    const char *user;
    const char *passwd;
    const char *db;
    unsigned int port;
    const char *unix_socket;

	LIST_ENTRY(pgsql_db)	rlist;
};

struct kore_mysql {
	u_int8_t state;
	unsigned long flags;
	char *error;
	MYSQL_RES *result;
	struct mysql_conn *conn;

	LIST_ENTRY(kore_mysql) rlist;
};

extern u_int16_t	mysql_conn_max;

void	kore_mysql_init(void);
int	kore_mysql_query_init(struct kore_mysql *, struct http_request *,
    const char *, const char *, const char *, const char *,
    unsigned int, const char *, unsigned long);
void	kore_mysql_handle(void *, int);
void	kore_mysql_cleanup(struct kore_mysql *);
void	kore_mysql_continue(struct http_request *, struct kore_mysql *);
int	kore_mysql_query(struct kore_mysql *, const char *);
int	kore_mysql_query_params(struct kore_mysql *,
	    const char *, int, u_int8_t, ...);
int	kore_mysql_register(const char *, const char *);
int	kore_mysql_ntuples(struct kore_mysql *);
void	kore_mysql_logerror(struct kore_mysql *);
void	kore_mysql_queue_remove(struct http_request *);
char	*kore_mysql_getvalue(struct kore_mysql *, int, int);
int	kore_mysql_getlength(struct kore_mysql *, int, int);

#if defined(__cplusplus)
}
#endif

#define KORE_MYSQL_STATE_INIT		1
#define KORE_MYSQL_STATE_WAIT		2
#define KORE_MYSQL_STATE_RESULT		3
#define KORE_MYSQL_STATE_ERROR		4
#define KORE_MYSQL_STATE_DONE		5
#define KORE_MYSQL_STATE_COMPLETE	6

#endif

