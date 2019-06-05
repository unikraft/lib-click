/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2014 NEC Europe Ltd,
 *               2019, NEC Laboratories Europe GmbH, NEC Corporation.
 *               All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */

/* To be implemented at a later point */
#define CLICK_CONSOLE_SUPPORT_IMPLEMENTED 0

extern "C"{
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
}

#include <click/config.h>
#include <click/lexer.hh>
#include <click/router.hh>
#include <click/master.hh>
#include <click/error.hh>
#include <click/timer.hh>
#include <click/string.hh>
#include <click/straccum.hh>
#include <click/driver.hh>

#include <uk/sched.h>
#include <uk/thread.h>

int click_nthreads = 1;

void *__dso_handle = NULL;

#define NLOG(fmt, ...)
#define LOG(fmt, ...) \
	printf("[%s:%d] " fmt "\n", \
		__FUNCTION__, __LINE__, ##__VA_ARGS__)

u_int _shutdown = 0;
u_int _reason = 0;

char*
event_token(char **key)
{
	char *p = *key, *p2;
	char *token;
	u_int sz;

	p2 = strchr(++p, '/');
	sz = (p2 != NULL ? p2 - p : strlen(p));
	token = strndup(p, sz);
	*key = (p+sz);

	return token;
}

char*
read_rid(char *path)
{
	char *id = strdup(path + 1);
	char *id_end = strchr(id, '/');
	*id_end = '\0';
	return id;
}

#if CLICK_CONSOLE_SUPPORT_IMPLEMENTED
String *
read_config(u_int rid = 0)
{
	char path[PATH_MAX_LEN];
	String *cfg = new String();
	char *token;

	for (int i = 0;; i++) {
		snprintf(path, PATH_MAX_LEN, "%s/%d/config/%d",
				PATH_ROOT, rid, i);
		xenbus_read(XBT_NIL, path, &token);
		if (token == NULL)
			break;
		*cfg += token;
	}

	return cfg;
}
#endif /* CLICK_CONSOLE_SUPPORT_IMPLEMENTED */
/*
 * click glue
 */

#define MAX_ROUTERS	64
static ErrorHandler *errh;
static Master master(1);

struct router_instance {
	Router *r;
	u_int f_stop;
} router_list[MAX_ROUTERS];

void
router_thread(void *thread_data)
{
	u_int *rid = (u_int*) thread_data;
	String *config = new String; //read_config(*rid);
	struct router_instance *ri = &router_list[*rid];

	ri->r = click_read_router(*config, true, errh, false, &master);
	if (ri->r->initialize(errh) < 0) {
		LOG("Router init failed!");
		ri->f_stop = 1;
		return;
	}

	ri->r->use();
	ri->r->activate(errh);

	LOG("Starting driver...\n\n");
	ri->r->master()->thread(0)->driver();

	LOG("Stopping driver...\n\n");
	ri->r->unuse();
	ri->f_stop = 1;

	LOG("Master/driver stopped, closing router_thread");
	free(config);
	free(rid);
}

void
router_stop(int n = MAX_ROUTERS)
{
	LOG("Stopping all routers...\n\n");
	for (int i = n - 1; i >= 0; --i) {
		Router *r = router_list[i].r;

		if (router_list[i].f_stop)
			continue;

		LOG("Stopping instance = %d...\n\n", i);

		do {
			r->please_stop_driver();
			uk_sched_yield();
		} while (!(router_list[i].f_stop));
	}
	LOG("Stopped all routers...\n\n");
}

#if CLICK_CONSOLE_SUPPORT_IMPLEMENTED
void
router_suspend(int n = MAX_ROUTERS)
{
}

void
router_resume(int n = MAX_ROUTERS)
{
}

u_int
on_status(int rid, char *key, void *data)
{
	String status = (char *) data;
	struct router_instance *r = &router_list[rid];

	LOG("router id %d", rid);
	LOG("status change to %s", data);
	if (status == "Running") {
		u_int *arg0;

		if (!r->f_stop)
			goto status_err;

		arg0 = (u_int*) malloc(sizeof(u_int));
		*arg0 = rid;
		r->f_stop = 0;
		create_thread((char*)"click", router_thread, arg0);
	} else if (status == "Halted") {
		if (r->f_stop)
			goto status_err;

		router_stop(rid);
	}

	return 0;
status_err:
	return -EINVAL;
}

static void
read_cname(char *path, Element **e, const Handler **h, int rid)
{
	int sep = strchr(path, '/') - path;
	String ename(path, sep), hname(path + sep + 1);
	Router *r = router_list[rid].r;

	NLOG("sep=%d element=%s handler=%s", sep,
			ename.c_str(), hname.c_str());

	*e = r->find(ename);
	*h = Router::handler(*e, hname);
	NLOG("ename %s=%p hname %s=%p", ename.c_str(), e, hname.c_str(), h);
}

u_int
on_elem_readh(int rid, char *key, void *data)
{
	Element *e;
	const Handler* h;
	String val, h_path, lock_path;
	int f_stop = router_list[rid].f_stop;

	if (strncmp(key, "/read/", 6))
		return 0;

	if (f_stop)
		return 0;

	read_cname(key+6, &e, &h, rid);

	if (!h || !h->readable())
		return EINVAL;

	val = h->call_read(e, "", ErrorHandler::default_handler());
	h_path = String(PATH_ROOT) + "/0/elements/" + e->name() + "/" + h->name();
	lock_path = h_path + "/lock";

	xenbus_write(XBT_NIL, h_path.c_str(), val.c_str());
	xenbus_write(XBT_NIL, lock_path.c_str(), "0");

	NLOG("element handler read %s", val.c_str());
	return 0;
}

u_int
on_elem_writeh(int rid, char *key, void *data)
{
	Element *e;
	const Handler* h;
	int f_stop = router_list[rid].f_stop;

	if (f_stop || strlen((char *) data) == 0)
		return 0;

	read_cname(key+1, &e, &h, rid);

	if (!h || !h->writable())
		return EINVAL;

	h->call_write(String(data), e, ErrorHandler::default_handler());
	NLOG("element handler write %s value %s", data, "");
	return 0;
}

u_int
on_router(char *key, void *data)
{
	char *p = key;
	String rev;
	u_int rid;

	if (strlen(key) == 0)
		return 0;

	rid = atoi(event_token(&p));
	rev = event_token(&p);

	if (rev == "control")
		on_elem_readh(rid, p, data);
	else if (rev == "elements")
		on_elem_writeh(rid, p, data);
	else if (rev == "status")
		on_status(rid, key, data);

	return 0;
}
#endif /* CLICK_CONSOLE_SUPPORT_IMPLEMENTED */

#if CONFIG_LIBCLICK_MAIN
#define CLICK_MAIN main
#else
#define CLICK_MAIN click_main
extern "C" int click_main(int argc, char **argv);
#endif

/* runs the event loop */
int CLICK_MAIN(int argc, char **argv)
{
	struct uk_thread *router;

	click_static_initialize();
	errh = ErrorHandler::default_handler();

	memset(router_list, 0, MAX_ROUTERS * sizeof(struct router_instance));
	for (int i = 0; i < MAX_ROUTERS; ++i) {
		router_list[i].f_stop = 1;
	}

#if CLICK_CONSOLE_SUPPORT_IMPLEMENTED
/* TODO: This interaction between xenbus and click should be replaced with a
 * generic unikraft interaction scheme, for example, via a console.
 */
	while (!_shutdown) {
		String value;
		char *err, *val, **path;

		path = xenbus_wait_for_watch_return(&xsdev->events);
		if (!path || _shutdown)
			continue;

		err = xenbus_read(XBT_NIL, *path, &val);
		if (err)
			continue;

		on_router((*path + strlen(PATH_ROOT)), val);
	}
#else
	router = uk_thread_create("click-router", router_thread, 0);
	uk_thread_wait(router);
#endif
	LOG("Shutting down...");

	return _reason;
}

/* app shutdown hook from minios */
/* TODO: could be reused as call-in from unikraft */
#if CLICK_CONSOLE_SUPPORT_IMPLEMENTED
extern "C" {
int app_shutdown(unsigned reason)
{
	switch (reason) {
	case SHUTDOWN_poweroff:
		LOG("Requested shutdown reason=poweroff");
		_shutdown = 1;
		_reason = reason;
		router_stop();
		break;
	case SHUTDOWN_reboot:
		LOG("Requested shutdown reason=reboot");
		_shutdown = 1;
		_reason = reason;
		router_stop();
		break;
	case SHUTDOWN_suspend:
		LOG("Requested shutdown reason=suspend");
		router_suspend();
		kernel_suspend();
		router_resume();
	default:
		LOG("Requested shutdown with invalid reason (%d)", reason);
		break;
	}

	return 0;
}
}
#endif /* CLICK_CONSOLE_SUPPORT_IMPLEMENTED */
