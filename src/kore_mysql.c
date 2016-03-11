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

#include <sys/param.h>
#include <sys/queue.h>

#include <kore/kore.h>
#include <kore/http.h>
#include "kore_mysql.h"

struct mysql_job {
	struct http_request	*req;
	struct kore_mysql	*mysql;

	TAILQ_ENTRY(mysql_job)	list;
};

struct mysql_wait {
	struct http_request		*req;
	TAILQ_ENTRY(mysql_wait)		list;
};

#define MYSQL_CONN_MAX		2
#define MYSQL_CONN_FREE		0x01
#define MYSQL_LIST_INSERTED	0x0100

static void	mysql_queue_wakeup(void);
static void	mysql_set_error(struct kore_mysql *, const char *);
static void	mysql_queue_add(struct http_request *);
static void	mysql_conn_release(struct kore_mysql *);
static void	mysql_conn_cleanup(struct mysql_conn *);
static void	mysql_read_result(struct kore_mysql *);
static void	mysql_schedule(struct kore_mysql *);

static struct mysql_conn	*mysql_conn_create(struct kore_mysql *,
				    struct mysql_db *);
static struct mysql_conn	*mysql_conn_next(struct kore_mysql *,
				    struct mysql_db *,
				    struct http_request *);

static struct kore_pool			mysql_job_pool;
static struct kore_pool			mysql_wait_pool;
static TAILQ_HEAD(, mysql_conn)		mysql_conn_free;
static TAILQ_HEAD(, mysql_wait)		mysql_wait_queue;
static LIST_HEAD(, mysql_db)		mysql_db_conn;
static u_int16_t			mysql_conn_count;
u_int16_t				mysql_conn_max = MYSQL_CONN_MAX;

void
kore_mysql_init(void)
{
	mysql_conn_count = 0;
	TAILQ_INIT(&mysql_conn_free);
	TAILQ_INIT(&mysql_wait_queue);
	LIST_INIT(&mysql_db_conn);

	kore_pool_init(&mysql_job_pool, "mysql_job_pool",
	    sizeof(struct mysql_job), 100);
	kore_pool_init(&mysql_wait_pool, "mysql_wait_pool",
	    sizeof(struct mysql_wait), 100);
}

int
kore_mysql_query_init(struct kore_mysql *mysql, struct http_request *req,
    const char *host, const char *user, const char *passwd, const char *dbname,
    unsigned int port, const char *unix_socket, 
    unsigned long client_flags, int connector_flags)
{
	struct mysql_db		*db;

	memset(mysql, 0, sizeof(*mysql));
	mysql->connector_flags = connector_flags;
	mysql->state = KORE_MYSQL_STATE_INIT;

	if ((req == NULL && (flags & KORE_MYSQL_ASYNC)) ||
	    ((flags & KORE_MYSQL_ASYNC) && (flags & KORE_MYSQL_SYNC))) {
		mysql_set_error(mysql, "Invalid query init parameters");
		return (KORE_RESULT_ERROR);
	}

	db = NULL;
	LIST_FOREACH(db, &mysql_db_conn, rlist) {
		if (!strcmp(db->host, host) ||
			!strcmp(db->user, user) ||
			!strcmp(db->passwd, passwd) ||
			!strcmp(db->dbname, dbname))
			break;
		if ((db->port != port) &&
			!strcmp(db->unix_socket, unix_socket))
			break;
	}

	if (db == NULL) {
		mysql_set_error(mysql, "No database found");
		return (KORE_RESULT_ERROR);
	}

	
	if ((mysql->conn = mysql_conn_next(mysql, db, req)) == NULL)
		return (KORE_RESULT_ERROR);

	if (mysql->flags & KORE_MYSQL_ASYNC) {
		mysql->conn->job = kore_pool_get(&mysql_job_pool);
		mysql->conn->job->req = req;
		mysql->conn->job->mysql = mysql;

		http_request_sleep(req);
		mysql->flags |= MYSQL_LIST_INSERTED;
		LIST_INSERT_HEAD(&(req->mysqls), mysql, rlist);
	}

	return (KORE_RESULT_OK);
}


int
kore_mysql_query(struct kore_mysql *mysql, const char *query)
{
	if (mysql->conn == NULL) {
		mysql_set_error(mysql, "No connection was set before query.");
		return (KORE_RESULT_ERROR);
	}

	if (mysql->flags & KORE_MYSQL_SYNC) {
		if (mysql_query(mysql->conn->mysql, query) != 0){
			mysql_set_error(mysql, mysql_error(mysql->conn->mysql));
			return (KORE_RESULT_ERROR);
		}
		mysql->result = mysql_store_result(mysql->conn->mysql)
		mysql->result = PQexec(mysql->conn->db, query);

		mysql->state = KORE_MYSQL_STATE_DONE;
	} else {
		if (mysql_query(mysql->conn->mysql, query) != 0) {
			mysql_set_error(mysql, mysql_error(mysql->conn->mysql));
			return (KORE_RESULT_ERROR);
		}

		mysql_schedule(mysql);
	}

	return (KORE_RESULT_OK);
}

int
kore_mysql_register(const char *dbname, const char *connstring)
{
	struct mysql_db		*mysqldb;

	LIST_FOREACH(mysqldb, &mysql_db_conn_strings, rlist) {
		if (!strcmp(mysqldb->name, dbname))
			return (KORE_RESULT_ERROR);
	}

	mysqldb = kore_malloc(sizeof(*mysqldb));
	mysqldb->name = kore_strdup(dbname);
	mysqldb->conn_string = kore_strdup(connstring);
	LIST_INSERT_HEAD(&mysql_db_conn_strings, mysqldb, rlist);

	return (KORE_RESULT_OK);
}

/*void
kore_mysql_handle(void *c, int err)
{
	struct http_request	*req;
	struct kore_mysql	*mysql;
	struct mysql_conn	*conn = (struct mysql_conn *)c;

	if (err) {
		mysql_conn_cleanup(conn);
		return;
	}

	req = conn->job->req;
	mysql = conn->job->mysql;
	kore_debug("kore_mysql_handle: %p (%d)", req, mysql->state);

	if (!PQconsumeInput(conn->db)) {
		mysql->state = KORE_MYSQL_STATE_ERROR;
		mysql->error = kore_strdup(PQerrorMessage(conn->db));
	} else {
		mysql_read_result(mysql);
	}

	if (mysql->state == KORE_MYSQL_STATE_WAIT) {
		http_request_sleep(req);
	} else {
		http_request_wakeup(req);
	}
}*/

/*void
kore_mysql_continue(struct http_request *req, struct kore_mysql *mysql)
{
	kore_debug("kore_mysql_continue: %p->%p (%d)",
	    req->owner, req, mysql->state);

	if (mysql->error) {
		kore_mem_free(mysql->error);
		mysql->error = NULL;
	}

	if (mysql->result) {
		PQclear(mysql->result);
		mysql->result = NULL;
	}

	switch (mysql->state) {
	case KORE_MYSQL_STATE_INIT:
	case KORE_MYSQL_STATE_WAIT:
		break;
	case KORE_MYSQL_STATE_DONE:
		http_request_wakeup(req);
		mysql_conn_release(mysql);
		break;
	case KORE_MYSQL_STATE_ERROR:
	case KORE_MYSQL_STATE_RESULT:
		kore_mysql_handle(mysql->conn, 0);
		break;
	default:
		fatal("unknown mysql state %d", mysql->state);
	}
}*/

void
kore_mysql_cleanup(struct kore_mysql *mysql)
{
	kore_debug("kore_mysql_cleanup(%p)", mysql);

	if (mysql->result != NULL)
		mysql_free_result(mysql->result);

	if (mysql->error != NULL)
		kore_mem_free(mysql->error);

	if (mysql->conn != NULL)
		mysql_conn_release(mysql);

	mysql->result = NULL;
	mysql->error = NULL;
	mysql->conn = NULL;

	if (mysql->flags & MYSQL_LIST_INSERTED) {
		LIST_REMOVE(mysql, rlist);
		mysql->flags &= ~MYSQL_LIST_INSERTED;
	}
}

void
kore_mysql_logerror(struct kore_mysql *mysql)
{
	kore_log(LOG_NOTICE, "MySQL error: %s",
	    (mysql->error) ? mysql->error : "unknown");
}

/*int
kore_mysql_ntuples(struct kore_mysql *mysql)
{
	return (PQntuples(mysql->result));
}

int
kore_mysql_getlength(struct kore_mysql *mysql, int row, int col)
{
	return (PQgetlength(mysql->result, row, col));
}

char *
kore_mysql_getvalue(struct kore_mysql *mysql, int row, int col)
{
	return (PQgetvalue(mysql->result, row, col));
}*/

void
kore_mysql_queue_remove(struct http_request *req)
{
	struct mysql_wait	*myw, *next;

	for (myw = TAILQ_FIRST(&mysql_wait_queue); myw != NULL; myw = next) {
		next = TAILQ_NEXT(myw, list);
		if (myw->req != req)
			continue;

		TAILQ_REMOVE(&mysql_wait_queue, myw, list);
		kore_pool_put(&mysql_wait_pool, myw);
		return;
	}
}

static struct mysql_conn *
mysql_conn_next(struct kore_mysql *mysql, struct mysql_db *db,
    struct http_request *req)
{
	struct mysql_conn	*conn;

	conn = NULL;

	TAILQ_FOREACH(conn, &mysql_conn_free, list) {
		if (!(conn->flags & MYSQL_CONN_FREE))
			fatal("got a mysql connection that was not free?");
		if (!strcmp(conn->name, db->name))
			break;
	}

	if (conn == NULL) {
		if (mysql_conn_count >= mysql_conn_max) {
			if (mysql->flags & KORE_MYSQL_ASYNC) {
				mysql_queue_add(req);
			} else {
				mysql_set_error(mysql,
				    "no available connection");
			}

			return (NULL);
		}

		if ((conn = mysql_conn_create(mysql, db)) == NULL)
			return (NULL);
	}

	conn->flags &= ~MYSQL_CONN_FREE;
	TAILQ_REMOVE(&mysql_conn_free, conn, list);

	return (conn);
}

static void
mysql_set_error(struct kore_mysql *mysql, const char *msg)
{
	if (mysql->error != NULL)
		kore_mem_free(mysql->error);

	mysql->error = kore_strdup(msg);
	mysql->state = KORE_MYSQL_STATE_ERROR;
}

static void
mysql_schedule(struct kore_mysql *mysql)
{
	int		fd;

	fd = PQsocket(mysql->conn->db);
	if (fd < 0)
		fatal("PQsocket returned < 0 fd on open connection");

	kore_platform_schedule_read(fd, mysql->conn);
	mysql->state = KORE_MYSQL_STATE_WAIT;
}

static void
mysql_queue_add(struct http_request *req)
{
	struct mysql_wait	*myw;

	http_request_sleep(req);

	myw = kore_pool_get(&mysql_wait_pool);
	myw->req = req;
	myw->req->flags |= HTTP_REQUEST_MYSQL_QUEUE;

	TAILQ_INSERT_TAIL(&mysql_wait_queue, myw, list);
}

static void
mysql_queue_wakeup(void)
{
	struct mysql_wait	*myw, *next;

	for (myw = TAILQ_FIRST(&mysql_wait_queue); myw != NULL; myw = next) {
		next = TAILQ_NEXT(myw, list);
		if (myw->req->flags & HTTP_REQUEST_DELETE)
			continue;

		http_request_wakeup(myw->req);
		myw->req->flags &= ~HTTP_REQUEST_MYSQL_QUEUE;

		TAILQ_REMOVE(&mysql_wait_queue, myw, list);
		kore_pool_put(&mysql_wait_pool, myw);
		return;
	}
}

static struct mysql_conn *
mysql_conn_create(struct kore_mysql *mysql, struct mysql_db *db)
{
	struct mysql_conn	*conn;

	if (db == NULL ||
		db->host == NULL ||
		db->user == NULL ||
		db->passwd == NULL ||
		db->dbname == NULL)
			if (db->port == 0 || db->unix_socket == NULL)
				fatal("mysql_conn_create: No connection data.");

	mysql_conn_count++;
	conn = kore_malloc(sizeof(*conn));
	kore_debug("mysql_conn_create(): %p", conn);

	conn->mysql = mysql_init(conn->mysql);
	if (conn->mysql == NULL){
		mysql_set_error(mysql, mysql_error(conn->mysql));
		mysql_conn_cleanup(conn);
		return (NULL);
	}
	
	if (mysql_real_connect(conn->mysql, db->host, db->user,
		db->passwd, db->dbname, db->port, db->unix_socket,
		db->flags) == NULL) {
		mysql_set_error(mysql, mysql_error(conn->mysql));
		mysql_conn_cleanup(conn);
		return (NULL);
	}	

	conn->job = NULL;
	conn->flags = MYSQL_CONN_FREE;
	conn->type = KORE_TYPE_MYSQL_CONN;
	conn->name = kore_strdup(db->name);
	TAILQ_INSERT_TAIL(&mysql_conn_free, conn, list);

	return (conn);
}

static void
mysql_conn_release(struct kore_mysql *mysql)
{
	int		fd;

	if (mysql->conn == NULL)
		return;

	/* Async query cleanup */
	if (mysql->flags & KORE_MYSQL_ASYNC) {
		if (mysql->conn != NULL) {
			fd = PQsocket(mysql->conn->db);
			kore_platform_disable_read(fd);
			kore_pool_put(&mysql_job_pool, mysql->conn->job);
		}
	}

	/* Drain just in case. */
	while (PQgetResult(mysql->conn->db) != NULL)
		;

	mysql->conn->job = NULL;
	mysql->conn->flags |= MYSQL_CONN_FREE;
	TAILQ_INSERT_TAIL(&mysql_conn_free, mysql->conn, list);

	mysql->conn = NULL;
	mysql->state = KORE_MYSQL_STATE_COMPLETE;

	mysql_queue_wakeup();
}

static void
mysql_conn_cleanup(struct mysql_conn *conn)
{
	struct http_request	*req;
	struct kore_mysql	*mysql;

	kore_debug("mysql_conn_cleanup(): %p", conn);

	if (conn->flags & MYSQL_CONN_FREE)
		TAILQ_REMOVE(&mysql_conn_free, conn, list);

	if (conn->job) {
		req = conn->job->req;
		mysql = conn->job->mysql;
		http_request_wakeup(req);

		mysql->conn = NULL;
		mysql_set_error(mysql, PQerrorMessage(conn->db));

		kore_pool_put(&mysql_job_pool, conn->job);
		conn->job = NULL;
	}

	if (conn->db != NULL)
		PQfinish(conn->db);

	mysql_conn_count--;
	kore_mem_free(conn->name);
	kore_mem_free(conn);
}

static void
mysql_read_result(struct kore_mysql *mysql)
{
	if (PQisBusy(mysql->conn->db)) {
		mysql->state = KORE_MYSQL_STATE_WAIT;
		return;
	}

	mysql->result = PQgetResult(mysql->conn->db);
	if (mysql->result == NULL) {
		mysql->state = KORE_MYSQL_STATE_DONE;
		return;
	}

	switch (PQresultStatus(mysql->result)) {
	case MYRES_COPY_OUT:
	case MYRES_COPY_IN:
	case MYRES_NONFATAL_ERROR:
	case MYRES_COPY_BOTH:
		break;
	case MYRES_COMMAND_OK:
		mysql->state = KORE_MYSQL_STATE_DONE;
		break;
	case MYRES_TUPLES_OK:
#if MY_VERSION_NUM >= 90200
	case MYRES_SINGLE_TUPLE:
#endif
		mysql->state = KORE_MYSQL_STATE_RESULT;
		break;
	case MYRES_EMPTY_QUERY:
	case MYRES_BAD_RESPONSE:
	case MYRES_FATAL_ERROR:
		mysql_set_error(mysql, PQresultErrorMessage(mysql->result));
		break;
	}
}
