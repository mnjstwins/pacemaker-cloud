/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * Authors:
 *  Steven Dake <sdake@redhat.com>
 *  Angus Salkeld <asalkeld@redhat.com>
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

#include "config.h"

#include <qb/qbdefs.h>
#include <qb/qblog.h>
#include <qmf/ConsoleSession.h>
#include <qmf/ConsoleEvent.h>
#include <string>
#include <iostream>
#include "mainloop.h"

#include "cpe_agent.h"
#include "cpe_httpd.h"
#include "cpe_impl.h"

using namespace std;
using namespace qmf;


CpeAgent::CpeAgent()
{
    this->http_port(8888);
    this->conductor_host("localhost");
    this->conductor_port(3000);
    this->conductor_auth("admin:password");
}

void CpeAgent::impl_set(CpeImpl *impl)
{
	this->impl = impl;
}

int
CpeAgent::console_handler(void)
{
	ConsoleEvent event;

	if (console_session->nextEvent(event)) {
		cout << "event" <<endl;
		if (event.getType() == CONSOLE_AGENT_ADD) {
			string extra;
			cout << "agent added" <<endl;
			if (event.getAgent().getName() == console_session->getConnectedBrokerAgent().getName()) {
				extra = "  [Connected Broker]";
				cout << "Agent Added: " << event.getAgent().getName() << extra << endl;
			}
		}
	}

	return QB_TRUE;
}

int
main(int argc, char **argv)
{
	CpeAgent agent;
	CpeImpl impl;
	int32_t rc;

	agent.impl_set(&impl);
	rc = agent.init(argc, argv, "cpe");

	CpeHttpd http(agent.http_port());
	http.impl_set(&impl);
	bool http_status = http.run();

	if (http_status) {
		agent.register_hook();
	}

	if (rc == 0) {
		agent.run();
	}
	return rc;
}

void
CpeAgent::setup(void)
{
	_cpe = qmf::Data(package.data_cpe);
	agent_session.addData(_cpe, "cpe");
}

bool
CpeAgent::event_dispatch(AgentEvent *event)
{
	const string& methodName(event->getMethodName());
	uint32_t rc = 0;
	string uuid;
	string monitor;

	switch (event->getType()) {
	case qmf::AGENT_METHOD:
		if (methodName == "deployable_start") {
			uuid = event->getArguments()["uuid"].asString();
			monitor = event->getArguments()["monitor"].asString();

			rc = impl->dep_start(uuid, monitor);

			event->addReturnArgument("rc", rc);

		} else if (methodName == "deployable_stop") {
			uuid = event->getArguments()["uuid"].asString();
			monitor = event->getArguments()["monitor"].asString();

			rc = impl->dep_stop(uuid, monitor);

			event->addReturnArgument("rc", rc);
		} else if (methodName == "deployable_reload") {
			uuid = event->getArguments()["uuid"].asString();
			monitor = event->getArguments()["monitor"].asString();

			rc = impl->dep_reload(uuid, monitor);

			event->addReturnArgument("rc", rc);
		}
		if (rc == 0) {
			agent_session.methodSuccess(*event);
		} else {
			agent_session.raiseException(*event, "oops");
		}
		break;

	default:
		break;
	}
	return true;
}
