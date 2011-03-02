/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * Authors: Angus Salkeld <asalkeld@redhat.com>
 *
 * This file is part of cpe.
 *
 * cpe is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * cpe is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with cpe.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <glib.h>
#include "upstart-dbus.h"

int
main (int argc, char **argv)
{
	GMainLoop *loop;

	/* Create a new event loop to run in */
	loop = g_main_loop_new (NULL, FALSE);

	upstart_init(loop);

	upstart_job_start("dped", "123-456-aaa");
//	upstart_job_start("dped", "789-012-bbb");

	/* Start the event loop */
//	g_main_loop_run (loop);

//	upstart_fini();
	return 0;
}

