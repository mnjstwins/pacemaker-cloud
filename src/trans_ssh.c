/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * Authors: Steven Dake <sdake@redhat.com>
 *          Angus Salkeld <asalkeld@redhat.com>
 *
 * This file is part of pacemaker-cloud.
 *
 * pacemaker-cloud is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * pacemaker-cloud is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pacemaker-cloud.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <inttypes.h>
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
#include <unistd.h>
#include <fcntl.h>

#include "cape.h"
#include "trans.h"

/*
 * Internal global variables
 */
static int ssh_init_rc = -1;

/*
 * Internal datatypes
 */
enum ssh_state {
	SSH_SESSION_CONNECTING = 1,
	SSH_SESSION_INIT = 2,
	SSH_SESSION_STARTUP = 3,
	SSH_USERAUTH_PUBLICKEY_FROMFILE = 4,
	SSH_KEEPALIVE_CONFIG = 5,
	SSH_SESSION_CONNECTED = 6
};

enum ssh_exec_state {
	SSH_CHANNEL_OPEN = 1,
	SSH_CHANNEL_EXEC = 2,
	SSH_CHANNEL_READ = 3,
	SSH_CHANNEL_READ_STDERR = 4,
	SSH_CHANNEL_SEND_EOF = 5,
	SSH_CHANNEL_WAIT_EOF = 6,
	SSH_CHANNEL_CLOSE = 7,
	SSH_CHANNEL_WAIT_CLOSED = 8,
	SSH_CHANNEL_FREE = 9
};

struct ssh_op {
	int ssh_rc;
	struct qb_list_head list;
	qb_loop_timer_handle ssh_timer;
	enum ssh_exec_state ssh_exec_state;
	void (*completion_func) (void *data, int rc);
	void (*timeout_func) (void *data);
	void *data;
	LIBSSH2_CHANNEL *channel;
	int failed;
	char *command;
	struct trans_ssh *transport;
};

struct ra_op {
	struct assembly *assembly;
	struct resource *resource;
	struct pe_operation *pe_op;
};

struct trans_ssh {
	int fd;
	enum ssh_state ssh_state;
	qb_loop_timer_handle keepalive_timer;
	LIBSSH2_SESSION *session;
	struct sockaddr_in sin;
	struct qb_list_head ssh_op_head;
	int scheduled;
	qb_loop_timer_handle healthcheck_timer;
};

/*
 * Internal implementation
 */

static void assembly_healthcheck(void *data);

static void assembly_ssh_exec(void *data);

static void transport_schedule(struct trans_ssh *trans_ssh)
{
	struct ssh_op *ssh_op;

	if (qb_list_empty(&trans_ssh->ssh_op_head) == 0) {
		trans_ssh->scheduled = 1;
		ssh_op = qb_list_entry(trans_ssh->ssh_op_head.next, struct ssh_op, list);
	assert(ssh_op);
		qb_loop_job_add(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);
	} else {
		trans_ssh->scheduled = 0;
	}
}

static void transport_unschedule(struct trans_ssh *trans_ssh)
{
	struct ssh_op *ssh_op;

	if (trans_ssh->scheduled) {
		trans_ssh->scheduled = 0;
		ssh_op = qb_list_entry(trans_ssh->ssh_op_head.next, struct ssh_op, list);
	assert(ssh_op);
		qb_loop_job_del(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);
	}
}

static void transport_failed(struct trans_ssh *trans_ssh)
{
	qb_enter();

	qb_loop_timer_del(NULL, trans_ssh->keepalive_timer);

	qb_leave();
}

static void ssh_op_complete(struct ssh_op *ssh_op)
{
	qb_loop_timer_del(NULL, ssh_op->ssh_timer);
	qb_list_del(&ssh_op->list);
	if (ssh_op->failed == 0) {
		ssh_op->completion_func(ssh_op->data, ssh_op->ssh_rc);
	}
	free(ssh_op->command);
	free(ssh_op);
}

static void assembly_ssh_exec(void *data)
{
	struct ssh_op *ssh_op = (struct ssh_op *)data;
	struct trans_ssh *trans_ssh = (struct trans_ssh *)ssh_op->transport;
	int rc;
	char buffer[4096];
	ssize_t rc_read;

	qb_enter();

	assert (trans_ssh->ssh_state == SSH_SESSION_CONNECTED);

	switch (ssh_op->ssh_exec_state) {
	case SSH_CHANNEL_OPEN:
		ssh_op->channel = libssh2_channel_open_session(trans_ssh->session);
		if (ssh_op->channel == NULL) {
			rc = libssh2_session_last_errno(trans_ssh->session);
			if (rc == LIBSSH2_ERROR_EAGAIN) {
				goto job_repeat_schedule;
			}
			qb_log(LOG_NOTICE,
				"open session failed %d\n", rc);
			qb_leave();
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
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_SEND_EOF;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_SEND_EOF:
		rc = libssh2_channel_send_eof(ssh_op->channel);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_send_eof failed %d\n", rc);
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_WAIT_EOF;

		/*
                 * no break here is intentional
		 */


	case SSH_CHANNEL_WAIT_EOF:
		rc = libssh2_channel_wait_eof(ssh_op->channel);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_wait_eof failed %d\n", rc);
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_CLOSE;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_CLOSE:
		rc = libssh2_channel_close(ssh_op->channel);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel close failed %d\n", rc);
			ssh_op->failed = 1;
			assert(0);
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_READ;

		/*
                 * no break here is intentional
		 */


	case SSH_CHANNEL_READ:
		rc_read = libssh2_channel_read(ssh_op->channel, buffer, sizeof(buffer));
		if (rc_read == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc_read < 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_read failed %"PRIu64, rc_read);
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_READ_STDERR;

		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_READ_STDERR:
		rc_read = libssh2_channel_read_stderr(ssh_op->channel, buffer, sizeof(buffer));
		if (rc_read == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc_read < 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_read_stderr failed %"PRIu64, rc_read);
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		ssh_op->ssh_exec_state = SSH_CHANNEL_WAIT_CLOSED;
		/*
                 * no break here is intentional
		 */

	case SSH_CHANNEL_WAIT_CLOSED:
		rc = libssh2_channel_wait_closed(ssh_op->channel);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_wait_closed failed %d\n", rc);
			assert(0);
			ssh_op->failed = 1;
			goto channel_free;
		}
		/*
		 * No ERROR_EAGAIN returned by following call
		 */
		ssh_op->ssh_rc = libssh2_channel_get_exit_status(ssh_op->channel);

		/*
                 * no break here is intentional
		 */

channel_free:
		ssh_op->ssh_exec_state = SSH_CHANNEL_FREE;

	case SSH_CHANNEL_FREE:
		rc = libssh2_channel_free(ssh_op->channel);
		if (rc == LIBSSH2_ERROR_EAGAIN) {
			goto job_repeat_schedule;
		}
		if (rc != 0) {
			qb_log(LOG_NOTICE,
				"libssh2_channel_free failed %d\n", rc);
		assert(0);
		}
		break;

	default:
		assert(0);
	} /* switch */

	ssh_op_complete(ssh_op);

	transport_schedule(trans_ssh);

	qb_leave();

	return;

job_repeat_schedule:
	assert(ssh_op);
	qb_loop_job_add(NULL, QB_LOOP_LOW, ssh_op, assembly_ssh_exec);

	qb_leave();
}

static void ssh_timeout(void *data)
{
	struct ssh_op *ssh_op = (struct ssh_op *)data;
	struct trans_ssh *trans_ssh = (struct trans_ssh *)ssh_op->transport;
	struct qb_list_head *list_temp;
	struct qb_list_head *list;
	struct ssh_op *ssh_op_del;

	qb_enter();

	transport_unschedule(trans_ssh);

	qb_log(LOG_NOTICE, "ssh timeout for command '%s'", ssh_op->command);
	qb_list_for_each_safe(list, list_temp, &trans_ssh->ssh_op_head) {
		ssh_op_del = qb_list_entry(list, struct ssh_op, list);
		qb_loop_timer_del(NULL, ssh_op_del->ssh_timer);
		qb_list_del(list);
		qb_log(LOG_NOTICE, "delete ssh operation '%s'", ssh_op_del->command);
	}

	ssh_op->timeout_func(ssh_op->data);

	qb_leave();
}

static void ssh_keepalive_send(void *data)
{
	struct trans_ssh *trans_ssh = (struct trans_ssh *)data;
	int seconds_to_next;

	qb_enter();

	libssh2_keepalive_send(trans_ssh->session, &seconds_to_next);
	qb_loop_timer_add(NULL, QB_LOOP_LOW, seconds_to_next * 1000 * QB_TIME_NS_IN_MSEC,
		trans_ssh, ssh_keepalive_send, &trans_ssh->keepalive_timer);

	qb_leave();
}

static void ssh_assembly_connect(void *data)
{
	struct assembly *assembly = (struct assembly *)data;
	struct trans_ssh *trans_ssh = (struct trans_ssh *)assembly->transport;
	char name[PATH_MAX];
	char name_pub[PATH_MAX];
	int rc;

	qb_enter();

	switch (trans_ssh->ssh_state) {
	case SSH_SESSION_INIT:
		trans_ssh->session = libssh2_session_init();
		if (trans_ssh->session == NULL) {
			goto job_repeat_schedule;
		}

		libssh2_session_set_blocking(trans_ssh->session, 0);
		trans_ssh->ssh_state = SSH_SESSION_STARTUP;

		/*
                 * no break here is intentional
		 */

	case SSH_SESSION_STARTUP:
		rc = libssh2_session_startup(trans_ssh->session, trans_ssh->fd);
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
		trans_ssh->ssh_state = SSH_USERAUTH_PUBLICKEY_FROMFILE;

	case SSH_USERAUTH_PUBLICKEY_FROMFILE:
		snprintf (name, PATH_MAX, "/var/lib/pacemaker-cloud/keys/%s",
			assembly->name);
		snprintf (name_pub, PATH_MAX, "/var/lib/pacemaker-cloud/keys/%s.pub",
			assembly->name);
		rc = libssh2_userauth_publickey_fromfile(trans_ssh->session,
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

		trans_ssh->ssh_state = SSH_KEEPALIVE_CONFIG;

	case SSH_KEEPALIVE_CONFIG:
		libssh2_keepalive_config(trans_ssh->session, 0, KEEPALIVE_TIMEOUT);
		ssh_keepalive_send(trans_ssh);

		/*
                 * no break here is intentional
		 */
		trans_ssh->ssh_state = SSH_SESSION_CONNECTED;

	case SSH_SESSION_CONNECTED:
		recover_state_set(&assembly->recover, RECOVER_STATE_RUNNING);
		assembly_healthcheck(assembly);
		break;
	case SSH_SESSION_CONNECTING:
		assert(0);
	}
error:
	qb_leave();
	return;

job_repeat_schedule:
	qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, ssh_assembly_connect);
	qb_leave();
}

static void assembly_healthcheck_failed(struct assembly *assembly)
{
	transport_failed(assembly->transport);
	recover_state_set(&assembly->recover, RECOVER_STATE_FAILED);
}

static void assembly_healthcheck_timeout(void *data) {
	struct assembly *assembly = (struct assembly *)data;

	assembly_healthcheck_failed(assembly);
}

static void assembly_healthcheck_completion(void *data, int ssh_rc)
{
	struct assembly *assembly = (struct assembly *)data;
	struct trans_ssh *trans_ssh;

	qb_enter();

	trans_ssh = (struct trans_ssh *)assembly->transport;

	qb_log(LOG_NOTICE, "assembly_healthcheck_completion for assembly '%s'", assembly->name);
	if (ssh_rc != 0) {
		qb_log(LOG_NOTICE, "assembly healthcheck failed %d\n", ssh_rc);
		transport_failed(trans_ssh);
		recover_state_set(&assembly->recover, RECOVER_STATE_FAILED);
		//free(ssh_op);

		qb_leave();
		return;
	}


	/*
	 * Add a healthcheck if asssembly is still running
	 */
	if (assembly->recover.state == RECOVER_STATE_RUNNING) {
		qb_log(LOG_NOTICE, "adding a healthcheck timer for assembly '%s'", assembly->name);
		qb_loop_timer_add(NULL, QB_LOOP_HIGH,
			HEALTHCHECK_TIMEOUT * QB_TIME_NS_IN_MSEC, assembly,
			assembly_healthcheck, &trans_ssh->healthcheck_timer);
	}

	qb_leave();
}

static void assembly_healthcheck(void *data)
{
	struct assembly *assembly = (struct assembly *)data;

	qb_enter();

	transport_execute(assembly->transport, assembly_healthcheck_completion,
		assembly_healthcheck_timeout, assembly, SSH_TIMEOUT, "uptime");

	qb_leave();
}

static void connect_execute(void *data)
{
	struct assembly *assembly = (struct assembly *)data;
	struct trans_ssh *trans_ssh = (struct trans_ssh *)assembly->transport;
	int rc;
	int flags;

	qb_enter();

	rc = connect(trans_ssh->fd, (struct sockaddr*)(&trans_ssh->sin),
		sizeof (struct sockaddr_in));
	if (rc == 0) {
		flags = fcntl(trans_ssh->fd, F_GETFL, 0);
		fcntl(trans_ssh->fd, F_SETFL, flags | (~O_NONBLOCK));
		qb_log(LOG_NOTICE, "Connected to assembly '%s'",
			assembly->name);
		trans_ssh->ssh_state = SSH_SESSION_INIT;
		qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, ssh_assembly_connect);
	} else {
		qb_loop_job_add(NULL, QB_LOOP_LOW, assembly, connect_execute);
	}

	qb_leave();
}

static int32_t
set_ocf_env_with_prefix(const char *key, void *value, void *user_data)
{
	char *buffer = (char*)user_data;
	strcat(buffer, "OCF_RESKEY_");
	strcat(buffer, (char *)key);
	strcat(buffer, "=");
	strcat(buffer, (char *)value);
	return 0;
}


void resource_action_completion(void *data, int ssh_rc)
{
	struct ra_op *ra_op = (struct ra_op *)data;
	enum ocf_exitcode pe_rc;

	qb_enter();

	if (strcmp(ra_op->pe_op->rclass, "lsb") == 0) {
		pe_rc = pe_resource_ocf_exitcode_get(ra_op->pe_op, ssh_rc);
	} else {
		pe_rc = ssh_rc;
	}

	resource_action_completed(ra_op->pe_op, pe_rc);
	pe_resource_unref(ra_op->pe_op);
	free(ra_op);

	qb_leave();
}

void resource_action_timeout(void *data)
{
	struct ra_op *ra_op = (struct ra_op *)data;
	qb_enter();

	recover_state_set(&ra_op->assembly->recover, RECOVER_STATE_FAILED);
	free(ra_op);

	qb_leave();
}

/*
 * External API
 */
void
transport_resource_action(struct assembly *assembly,
		   struct resource *resource,
		   struct pe_operation *pe_op)
{
	char envs[RESOURCE_ENVIRONMENT_MAX];
	struct ra_op *ra_op;

	qb_enter();

	ra_op = calloc(1, sizeof (struct ra_op));
	ra_op->assembly = assembly;
	ra_op->resource = resource;
	ra_op->pe_op = pe_op;

	pe_resource_ref(pe_op);

	if (strcmp(pe_op->rclass, "lsb") == 0) {
		/*
		 * LSB resource class
		 */
		if (strcmp(pe_op->method, "monitor") == 0) {
			transport_execute(
				assembly->transport,
				resource_action_completion,
				resource_action_timeout,
				ra_op,
				SSH_TIMEOUT,
				"systemctl status %s.service",
				ra_op->pe_op->rtype);
		} else {
			transport_execute(
				assembly->transport,
				resource_action_completion,
				resource_action_timeout,
				ra_op,
				SSH_TIMEOUT,
				"systemctl %s %s.service",
				ra_op->pe_op->method, ra_op->pe_op->rtype);
		}
	} else {
		/*
		 * OCF resource class
		 */
		sprintf(envs, "OCF_RA_VERSION_MAJOR=1 OCF_RA_VERSION_MINOR=0 OCF_ROOT=%s", OCF_ROOT);

		if (pe_op->rname) {
			strcat(envs, "OCF_RESOURCE_INSTANCE=");
			strcat(envs, pe_op->rname);
		}

		if (pe_op->rtype != NULL) {
			strcat(envs, "OCF_RESOURCE_TYPE=");
			strcat(envs, pe_op->rtype);
		}

		if (pe_op->rprovider != NULL) {
			strcat(envs, "OCF_RESOURCE_PROVIDER=");
			strcat(envs, pe_op->rprovider);
		}
		if (pe_op->params) {
			qb_map_foreach(pe_op->params, set_ocf_env_with_prefix, envs);
		}
		transport_execute(
			assembly->transport,
			resource_action_completion,
			resource_action_timeout,
			ra_op,
			SSH_TIMEOUT,
			"%s %s/resource.d/%s/%s %s )",
			envs, OCF_ROOT, pe_op->rprovider,
			pe_op->rtype, pe_op->method);
	}

	qb_leave();
}

void*
transport_connect(struct assembly * a)
{
	unsigned long hostaddr;
        struct trans_ssh *trans_ssh;
	int flags;

	qb_enter();

	if (ssh_init_rc != 0) {
		ssh_init_rc = libssh2_init(0);
	}
	assert(ssh_init_rc == 0);

	trans_ssh = calloc(1, sizeof(struct trans_ssh));
	a->transport = trans_ssh;

	hostaddr = inet_addr(a->address);
	trans_ssh->fd = socket(AF_INET, SOCK_STREAM, 0);
	flags = fcntl(trans_ssh->fd, F_GETFL, 0);
	fcntl(trans_ssh->fd, F_SETFL, flags | O_NONBLOCK);
	trans_ssh->sin.sin_family = AF_INET;
	trans_ssh->sin.sin_port = htons(22);
	trans_ssh->sin.sin_addr.s_addr = hostaddr;
	trans_ssh->ssh_state = SSH_SESSION_CONNECTING;
	qb_list_init(&trans_ssh->ssh_op_head);

	qb_log(LOG_NOTICE, "Connection in progress to assembly '%s'",
		a->name);

	qb_loop_job_add(NULL, QB_LOOP_LOW, a, connect_execute);

	qb_leave();
	return trans_ssh;
}

void transport_disconnect(struct assembly *a)
{
	struct trans_ssh *trans_ssh = (struct trans_ssh *)a->transport;
	struct qb_list_head *list_temp;
	struct qb_list_head *list;
	struct ssh_op *ssh_op_del;

	qb_enter();

	if (trans_ssh == NULL) {
		return;
	}

	qb_loop_timer_del(NULL, trans_ssh->healthcheck_timer);

	/*
	 * Delete a transport connection in progress
	 */
	if (trans_ssh->ssh_state == SSH_SESSION_INIT) {
		qb_loop_job_del(NULL, QB_LOOP_LOW, a, connect_execute);
	}

	/*
	 * Delete a transport attempting an SSH connection
	 */
	if (trans_ssh->ssh_state != SSH_SESSION_CONNECTED) {
		qb_loop_job_del(NULL, QB_LOOP_LOW, a, ssh_assembly_connect);
	}

	if (trans_ssh->ssh_state == SSH_SESSION_CONNECTED) {
		transport_unschedule(trans_ssh);
	}

	/*
	 * Delete any outstanding ssh operations
	 */
	qb_list_for_each_safe(list, list_temp, &trans_ssh->ssh_op_head) {
		ssh_op_del = qb_list_entry(list, struct ssh_op, list);

		qb_log(LOG_NOTICE, "delete ssh operation '%s'", ssh_op_del->command);

		qb_loop_timer_del(NULL, ssh_op_del->ssh_timer);
		qb_list_del(list);
		libssh2_channel_free(ssh_op_del->channel);
		free(ssh_op_del->command);
		free(ssh_op_del);
	}
	/*
	 * Free the SSH session associated with this transport
	 * there are no breaks intentionally in this switch
	 */
	switch (trans_ssh->ssh_state) {
	case SSH_SESSION_CONNECTED:
	case SSH_SESSION_STARTUP:
		libssh2_session_free(trans_ssh->session);
	case SSH_USERAUTH_PUBLICKEY_FROMFILE:
	case SSH_KEEPALIVE_CONFIG:
		qb_loop_timer_del(NULL, trans_ssh->keepalive_timer);
	case SSH_SESSION_INIT:
		qb_loop_job_del(NULL, QB_LOOP_LOW, a, ssh_assembly_connect);
	default:
		break;
	}

	free(a->transport);
	close(trans_ssh->fd);
	qb_leave();
}

void
transport_execute(void *transport,
	void (*completion_func)(void *data, int ssh_rc),
	void (*timeout_func)(void *data),
	void *data,
	uint64_t timeout_msec,
	char *format, ...)
{
	va_list ap;
	struct trans_ssh *trans_ssh = (struct trans_ssh *)transport;
	struct ssh_op *ssh_op;
	char ssh_command_buffer[COMMAND_MAX];

	qb_enter();

	assert(transport);
	/*
	 * Only execute an opperation when in the connected state
	 */
	if (trans_ssh->ssh_state != SSH_SESSION_CONNECTED) {
		qb_leave();
		return;
	}
	ssh_op = calloc(1, sizeof(struct ssh_op));

	va_start(ap, format);
	vsnprintf(ssh_command_buffer, COMMAND_MAX, format, ap);
	va_end(ap);
	ssh_op->command = strdup(ssh_command_buffer);

	qb_log(LOG_NOTICE, "transport_exec command '%s'", ssh_command_buffer);

	ssh_op->ssh_rc = 0;
	ssh_op->failed = 0;
	ssh_op->data = data;
	ssh_op->transport = transport;
	ssh_op->ssh_exec_state = SSH_CHANNEL_OPEN;
	ssh_op->completion_func = completion_func;
	ssh_op->timeout_func = timeout_func;
	qb_list_init(&ssh_op->list);
	qb_list_add_tail(&ssh_op->list, &trans_ssh->ssh_op_head);

	if (trans_ssh->scheduled == 0) {
		transport_schedule(transport);
	}

	qb_loop_timer_add(NULL, QB_LOOP_LOW,
		timeout_msec * QB_TIME_NS_IN_MSEC,
		ssh_op, ssh_timeout, &ssh_op->ssh_timer);
	qb_leave();
}
