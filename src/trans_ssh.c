#include <sys/epoll.h>
#include <qb/qbdefs.h>
#include <qb/qblist.h>
#include <qb/qbloop.h>
#include <qb/qblog.h>
#include <qb/qbmap.h>
#include <libssh2.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <arpa/inet.h>


#include "cape.h"
#include "trans.h"

#define KEEPALIVE_TIMEOUT 15		/* seconds */
#define SSH_TIMEOUT 5000		/* milliseconds */
#define SCHEDULE_PROCESS_TIMEOUT 1000	/* milliseconds */
#define PENDING_TIMEOUT 1000		/* milliseconds */
#define HEALTHCHECK_TIMEOUT 3000	/* milliseconds */

enum ssh_state {
	SSH_SESSION_INIT = 0,
	SSH_SESSION_STARTUP = 1,
	SSH_KEEPALIVE_CONFIG = 2,
	SSH_USERAUTH_PUBLICKEY_FROMFILE = 3,
	SSH_CONNECTED = 4
};

struct ta_ssh {
	int fd;
	enum ssh_state ssh_state;
	qb_loop_timer_handle keepalive_timer;
	LIBSSH2_SESSION *session;
	struct sockaddr_in sin;
	struct qb_list_head ssh_op_head;
};

static void assembly_ssh_exec(void *data)
{
	struct ssh_operation *ssh_op = (struct ssh_operation *)data;
	struct ta_ssh *ta_ssh = (struct ta_ssh *)ssh_op->assembly->transport_assembly;
	int rc;
	int rc_close;
	char buffer[4096];

	switch (ssh_op->ssh_exec_state) {
	case SSH_CHANNEL_OPEN:
		ssh_op->channel = libssh2_channel_open_session(ta_ssh->session);
		if (ssh_op->channel == NULL) {
			rc = libssh2_session_last_errno(ta_ssh->session);
			if (rc == LIBSSH2_ERROR_EAGAIN) {
				goto job_repeat_schedule;
			}
			qb_log(LOG_NOTICE,
				"open session failed %d\n", rc);
			return;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_EXEC;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_EXEC:
		rc = libssh2_channel_exec(ssh_op->channel, ssh_op->command);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_exec failed %d\n", rc);
			goto error_close;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_READ;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_READ:
		rc = libssh2_channel_read(ssh_op->channel, buffer, sizeof(buffer));
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc < 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_read failed %d\n", rc);
			goto error_close;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_CLOSE;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_CLOSE:
		rc_close = libssh2_channel_close(ssh_op->channel);
		if (rc_close == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc_close != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel close failed %d\n", rc_close);
			return;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_WAIT_CLOSED;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_WAIT_CLOSED:
		rc_close = libssh2_channel_wait_closed(ssh_op->channel);
		if (rc_close == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc_close != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_wait_closed failed %d\n", rc_close);
			return;
		}
		ssh_op->ssh_rc = libssh2_channel_get_exit_status(ssh_op->channel);
		ssh_op->ssh_exec_state = SSH_CHANNEL_FREE;

		/*
                 * no break here is intentional
		 */

error_close:
	case SSH_CHANNEL_FREE:
		rc = libssh2_channel_free(ssh_op->channel);
		if (rc < 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_free failed %d\n", rc);
		}
		/*
                 * no break here is intentional
		 */
	} /* switch */

	qb_loop_timer_del(NULL, ssh_op->ssh_timer);
	qb_loop_job_add(NULL, QB_LOOP_LOW, ssh_op, ssh_op->completion_func);
	qb_list_del(&ssh_op->list);
	return;

job_repeat_schedule:
	qb_loop_job_add(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);
}

static void ssh_timeout(void *data)
{
	struct ssh_operation *ssh_op = (struct ssh_operation *)data;
	struct ta_ssh *ta_ssh = (struct ta_ssh *)ssh_op->assembly->transport_assembly;
	struct qb_list_head *list_temp;
	struct qb_list_head *list;
	struct ssh_operation *ssh_op_del;

	qb_log(LOG_NOTICE, "ssh service timeout on assembly '%s'", ssh_op->assembly->name);
	qb_list_for_each_safe(list, list_temp, &ta_ssh->ssh_op_head) {
		ssh_op_del = qb_list_entry(list, struct ssh_operation, list);
		qb_loop_timer_del(NULL, ssh_op_del->ssh_timer);
		qb_loop_job_del(NULL, QB_LOOP_LOW, ssh_op_del,
			assembly_ssh_exec);
		if (ssh_op_del->resource) {
			qb_loop_timer_del(NULL, ssh_op_del->resource->monitor_timer);
		}
		qb_list_del(list);
		qb_log(LOG_NOTICE, "delete ssh operation '%s'", ssh_op_del->command);
	}

	assembly_state_changed(ssh_op->assembly, INSTANCE_STATE_FAILED);
}

static void ssh_keepalive_send(void *data)
{
	struct ta_ssh *ta_ssh = (struct ta_ssh *)data;
	int seconds_to_next;

	libssh2_keepalive_send(ta_ssh->session, &seconds_to_next);
	qb_loop_timer_add(NULL, QB_LOOP_LOW, seconds_to_next * 1000 * QB_TIME_NS_IN_MSEC,
		ta_ssh, ssh_keepalive_send, &ta_ssh->keepalive_timer);
}

static void ssh_assembly_connect(void *data)
{
	struct assembly *assembly = (struct assembly *)data;
	struct ta_ssh *ta_ssh = (struct ta_ssh *)assembly->transport_assembly;
	char name[1024];
	char name_pub[1024];
	int rc;

	switch (ta_ssh->ssh_state) {
	case SSH_SESSION_INIT:
		ta_ssh->session = libssh2_session_init();
		if (ta_ssh->session == NULL) {
			rc = libssh2_session_last_errno(ta_ssh->session);
			if (rc == LIBSSH2_ERROR_EAGAIN) {
				goto job_repeat_schedule;
			}
			qb_log(LOG_NOTICE,
				"session init failed %d\n", rc);
		}

		libssh2_session_set_blocking(ta_ssh->session, 0);
		ta_ssh->ssh_state = SSH_SESSION_STARTUP;

		/*
                 * no break here is intentional
		 */

	case SSH_SESSION_STARTUP:
		rc = libssh2_session_startup(ta_ssh->session, ta_ssh->fd);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"session startup failed %d\n", rc);
			goto error;
		}

		/*
                 * no break here is intentional
		 */
		ta_ssh->ssh_state = SSH_KEEPALIVE_CONFIG;

	case SSH_KEEPALIVE_CONFIG:
		libssh2_keepalive_config(ta_ssh->session, 1, KEEPALIVE_TIMEOUT);
		qb_loop_job_add(NULL, QB_LOOP_LOW, ta_ssh, ssh_keepalive_send);

		/*
                 * no break here is intentional
		 */
		ta_ssh->ssh_state = SSH_USERAUTH_PUBLICKEY_FROMFILE;

	case SSH_USERAUTH_PUBLICKEY_FROMFILE:
		sprintf (name, "/var/lib/pacemaker-cloud/keys/%s",
			assembly->name);
		sprintf (name_pub, "/var/lib/pacemaker-cloud/keys/%s.pub",
			assembly->name);
		rc = libssh2_userauth_publickey_fromfile(ta_ssh->session,
			"root", name_pub, name, "");
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc) {
			qb_log(LOG_ERR,
				"Authentication by public key for '%s' failed\n",
				assembly->name);
		} else {
			qb_log(LOG_NOTICE,
				"Authentication by public key for '%s' successful\n",
				assembly->name);
		}
		assembly_healthcheck(assembly);

		assembly_state_changed(assembly, INSTANCE_STATE_RUNNING);
		ta_ssh->ssh_state = SSH_CONNECTED;

	case SSH_CONNECTED:
		break;
	}
error:
	return;

job_repeat_schedule:
	qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, ssh_assembly_connect);
}

static void connect_execute(void *data)
{
	struct assembly *assembly = (struct assembly *)data;
	struct ta_ssh *ta_ssh = (struct ta_ssh *)assembly->transport_assembly;
	int rc;

	rc = connect(ta_ssh->fd, (struct sockaddr*)(&ta_ssh->sin),
		sizeof (struct sockaddr_in));
	if (rc == 0) {
		qb_log(LOG_NOTICE, "Connected to assembly '%s'",
			assembly->name);
		qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, ssh_assembly_connect);
	} else {
		qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, connect_execute);
	}
}

void assembly_connect(struct assembly *assembly)
{
	struct ta_ssh *ta_ssh = (struct ta_ssh *)assembly->transport_assembly;
	unsigned long hostaddr;

	hostaddr = inet_addr(assembly->address);
	ta_ssh->fd = socket(AF_INET, SOCK_STREAM, 0);
	ta_ssh->sin.sin_family = AF_INET;
	ta_ssh->sin.sin_port = htons(22);
	ta_ssh->sin.sin_addr.s_addr = hostaddr;
	ta_ssh->ssh_state = SSH_SESSION_INIT;

	qb_log(LOG_NOTICE, "Connection in progress to assembly '%s'",
		assembly->name);

	qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, connect_execute);
}

void ssh_nonblocking_exec(struct assembly *assembly,
	struct resource *resource, struct pe_operation *op,
	void (*completion_func)(void *),
	char *format, ...)
{
	va_list ap;
	struct ssh_operation *ssh_op;
	struct ta_ssh *ta_ssh;

	ssh_op = calloc(1, sizeof(struct ssh_operation));

	va_start(ap, format);
	vsprintf(ssh_op->command, format, ap);
	va_end(ap);

	qb_log(LOG_NOTICE, "ssh_exec for assembly '%s' command '%s'",
		assembly->name, ssh_op->command);

	ssh_op->assembly = assembly;
	ssh_op->ssh_rc = 0;
	ssh_op->op = op;
	ssh_op->ssh_exec_state = SSH_CHANNEL_OPEN;
	ssh_op->resource = resource;
	ssh_op->completion_func = completion_func;
	qb_list_init(&ssh_op->list);
	ta_ssh = ssh_op->assembly->transport_assembly;
	qb_list_add_tail(&ssh_op->list, &ta_ssh->ssh_op_head);
	qb_loop_job_add(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);
	qb_loop_timer_add(NULL, QB_LOOP_LOW,
		SSH_TIMEOUT * QB_TIME_NS_IN_MSEC,
		ssh_op, ssh_timeout, &ssh_op->ssh_timer);
}


void ssh_op_delete(struct ssh_operation *ssh_op)
{
	qb_loop_job_del(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);
}

void *ta_alloc_init(void) {
        struct ta_ssh *ta_ssh;

        ta_ssh = calloc(1, sizeof(struct ta_ssh));
	qb_list_init(&ta_ssh->ssh_op_head);
	ta_ssh->ssh_state = SSH_SESSION_INIT;

	return ta_ssh;
}

void ta_del(void *ta) 
{
	struct ta_ssh *ta_ssh = (struct ta_ssh *)ta;
	qb_loop_timer_del(NULL, ta_ssh->keepalive_timer);
}
