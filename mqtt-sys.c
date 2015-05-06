/*
 * Copyright (c) 2015 Jan-Piet Mens <jpmens()gmail.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <getopt.h>
#include <sys/utsname.h>
#include <signal.h>
#include <mosquitto.h>
#include <errno.h>
#include "uthash.h"
#include "utarray.h"

#ifndef TRUE
# define TRUE (1)
#endif
#ifndef FALSE
# define FALSE (0)
#endif

#define PROGNAME "mqtt-sys"

#define DIM(x) 	( sizeof(x) / sizeof(x[0]) )

static struct _metrics {
    const char *metric;         /* collectd metric name */
    const char *type;		/* collectd type: https://github.com/astro/collectd/blob/master/src/types.db*/
    const char *topic;          /* MQTT topic name */
} metrics[] = {
#include "metrics.h"
};

struct tname {
    const char *topic;          /* MQTT topic */
    const char *metric;         /* metric name for collectd */
    const char *type;           /* metric type for collectd */
    UT_hash_handle hh;
};
static struct tname *map = NULL;

static struct mosquitto *m = NULL;

struct udata {
	char *nodename;
};

/*
 * Load the uthash table with topic -> metric if, and only if,
 * metric is not NULL
 */

void loadhashtable()
{
    struct _metrics *mm = metrics;
    struct tname *t;
    int i;

    for (i = 0; i < DIM(metrics); mm = &metrics[++i]) {
    	if (mm->metric) {
		//  printf("%s\n", mm->metric);
		t = (struct tname *)malloc(sizeof(struct tname));
		t->topic  = mm->topic;
		t->metric = mm->metric;
		t->type	  = mm->type;
		HASH_ADD_KEYPTR( hh, map, t->topic, strlen(t->topic), t );
	}
    }


}

void catcher(int sig)
{
	fprintf(stderr, "Going down on signal %d\n", sig);

	if (m) {
		mosquitto_disconnect(m);
		mosquitto_loop_stop(m, false);
		mosquitto_lib_cleanup();
	}
	exit(1);
}

void fatal(void)
{
	if (m) {
		mosquitto_disconnect(m);
		mosquitto_loop_stop(m, false);
		mosquitto_lib_cleanup();
	}
	exit(1);
}

void cb_sub(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg)
{
	char *topic = msg->topic;
	char *payload = msg->payload;
	struct tname *t;
	struct udata *ud = (struct udata *)userdata;
	time_t now;
	double val;

	/*
	 * If the topic is not in our hash, return (i.e. ignore this message)
	 */

	HASH_FIND_STR(map, topic, t);
	if (!t) {
		return;
	}

	time(&now);
	val = atof(payload);

	// printf("%s == %s  %s\n", t->metric, t->type, payload);
	printf("PUTVAL %s/%s/%s-%s %ld:%.2lf\n",
		ud->nodename,
		PROGNAME,
		t->type,
		t->metric,
		now,
		val);

}

void cb_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
	if (rc == 0) {
		// Disconnect requested by client
	} else {
		fprintf(stderr, "%s: disconnected: reason: %d (%s)\n",
			PROGNAME, rc, strerror(errno));
		fatal();
	}
}

int main(int argc, char **argv)
{
	char ch, *progname = *argv;
	int usage = 0, rc;
	struct utsname uts;
	char clientid[80];
	char *host = "localhost", *ca_file;
	int port = 1883, keepalive = 60;
	int do_tls = FALSE, tls_insecure = FALSE;
	int do_psk = FALSE;
	int have_host = FALSE;
	char *psk_key = NULL, *psk_identity = NULL;
	char *nodename, *username = NULL, *password = NULL;
	char *collectdnode = NULL, **t;
	struct udata udata;
	UT_array *topics;


	setvbuf(stdout, NULL, _IONBF, 0);

	utarray_new(topics, &ut_str_icd);

	while ((ch = getopt(argc, argv, "i:t:h:p:C:u:P:K:N:I:")) != EOF) {
		switch (ch) {
			case 'C':
				ca_file = optarg;
				do_tls = TRUE;
				break;
			case 'h':
				host = strdup(optarg);
				have_host = TRUE;
				break;
			case 's':
				tls_insecure = TRUE;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'I':
				psk_identity = optarg;
				do_psk = TRUE;
				break;
			case 'N':
				collectdnode = strdup(optarg);
				break;
			case 't':
				utarray_push_back(topics, &optarg);
				break;
			case 'u':
				username = strdup(optarg);
				break;
			case 'P':
				password = strdup(optarg);
				break;
			case 'K':
				psk_key = optarg;
				do_psk = TRUE;
				break;
			default:
				usage = 1;
				break;
		}
	}

	if (do_tls && do_psk)
		usage = 1;
	if (do_psk && (psk_key == NULL || psk_identity == NULL))
		usage = 1;

	if (usage) {
		fprintf(stderr, "Usage: %s [-h host] [-p port] [-C CA-cert] [-u username] [-P password] [-K psk-key] [-I psk-identity] [-s] [-N nodename]\n", progname);
		exit(1);
	}

	loadhashtable();

	/* Determine nodename: either use the -h value of the MQTT broker
	 * or get local nodename */

	if (have_host) {
		nodename = host;
	} else {

		if (uname(&uts) == 0) {
			char *p;
			nodename = strdup(uts.nodename);

			if ((p = strchr(nodename, '.')) != NULL)
				*p = 0;
		} else {
			nodename = strdup("unknown");
		}
	}

	nodename = (collectdnode) ? collectdnode : nodename;

	mosquitto_lib_init();

	udata.nodename = nodename;

	sprintf(clientid, "%s-%d", PROGNAME, getpid());
	m = mosquitto_new(clientid, TRUE, (void *)&udata);
	if (!m) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}

	if (do_psk) {
		rc = mosquitto_tls_psk_set(m, psk_key, psk_identity,NULL);
		if (rc != MOSQ_ERR_SUCCESS) {
			fprintf(stderr, "Cannot set TLS PSK: %s\n",
				mosquitto_strerror(rc));
			exit(3);
		}
	} else if (do_tls) {
		rc = mosquitto_tls_set(m, ca_file, NULL, NULL, NULL, NULL);
		if (rc != MOSQ_ERR_SUCCESS) {
			fprintf(stderr, "Cannot set TLS PSK: %s\n",
				mosquitto_strerror(rc));
			exit(3);
		}

		/* FIXME */
		// mosquitto_tls_opts_set(m, SSL_VERIFY_PEER, "tlsv1", NULL);
		
		if (tls_insecure) {
#if LIBMOSQUITTO_VERSION_NUMBER >= 1002000
			/* mosquitto_tls_insecure_set() requires libmosquitto 1.2. */
			mosquitto_tls_insecure_set(m, TRUE);
#endif
		}
	}

	if (username) {
		mosquitto_username_pw_set(m, username, password);
	}

	mosquitto_message_callback_set(m, cb_sub);
	mosquitto_disconnect_callback_set(m, cb_disconnect);
	if ((rc = mosquitto_connect(m, host, port, keepalive)) != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "Unable to connect to %s:%d: %s\n", host, port,
			mosquitto_strerror(rc));
		perror("");
		exit(2);
	}

	signal(SIGINT, catcher);

	if (utarray_len(topics) == 0) {
		char *sys = "$SYS/#";

		utarray_push_back(topics, &sys);
	}

	t = NULL;
	while ((t = (char **)utarray_next(topics, t))) {
		// printf("sub to %s\n", *t);
		mosquitto_subscribe(m, NULL, *t, 0);
	}

	utarray_free(topics);

	while (1) {
		int rc = mosquitto_loop(m, -1, 1);
		if (rc) {
			sleep(15);
			mosquitto_reconnect(m);
		}
	}

	free(nodename);

	mosquitto_disconnect(m);
	mosquitto_lib_cleanup();

	return 0;
}
