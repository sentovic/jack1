/*
    Copyright (C) 2001-2003 Paul Davis
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <math.h>
#include <unistd.h>
#include <sys/socket.h>

#if defined(linux) 
    #include <sys/poll.h>
#elif defined(__APPLE__) && defined(__POWERPC__) 
    #include "fakepoll.h"
    #include "ipc.h"
#endif

#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ipc.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <limits.h>

#include <config.h>

#include <jack/internal.h>
#include <jack/engine.h>
#include <jack/driver.h>
#include <jack/time.h>
#include <jack/version.h>
#include <jack/shm.h>

#ifdef USE_CAPABILITIES
/* capgetp and capsetp are linux only extensions, not posix */
#undef _POSIX_SOURCE
#include <sys/capability.h>
#endif

#define JACK_ERROR_WITH_SOCKETS 10000000

typedef struct {

    jack_port_internal_t *source;
    jack_port_internal_t *destination;

} jack_connection_internal_t;

typedef struct _jack_driver_info {
    jack_driver_t *(*initialize)(jack_client_t*, int, char**);
    void           (*finish);
    char           (*client_name);
    dlhandle       handle;
} jack_driver_info_t;

static int                    jack_port_assign_buffer (jack_engine_t *, jack_port_internal_t *);
static jack_port_internal_t *jack_get_port_by_name (jack_engine_t *, const char *name);

static void jack_zombify_client (jack_engine_t *engine, jack_client_internal_t *client);
static void jack_remove_client (jack_engine_t *engine, jack_client_internal_t *client);
static void jack_client_delete (jack_engine_t *, jack_client_internal_t *);

static jack_client_internal_t *jack_client_internal_new (jack_engine_t *engine, int fd, jack_client_connect_request_t *);
static jack_client_internal_t *jack_client_internal_by_id (jack_engine_t *engine, jack_client_id_t id);

static void jack_sort_graph (jack_engine_t *engine);
static int  jack_rechain_graph (jack_engine_t *engine);
static int  jack_get_fifo_fd (jack_engine_t *engine, unsigned int which_fifo);
static void jack_clear_fifos (jack_engine_t *engine);

static int  jack_port_do_connect (jack_engine_t *engine, const char *source_port, const char *destination_port);
static int  jack_port_do_disconnect (jack_engine_t *engine, const char *source_port, const char *destination_port);
static int  jack_port_do_disconnect_all (jack_engine_t *engine, jack_port_id_t);

static int  jack_port_do_unregister (jack_engine_t *engine, jack_request_t *);
static int  jack_port_do_register (jack_engine_t *engine, jack_request_t *);
static int  jack_do_get_port_connections (jack_engine_t *engine, jack_request_t *req, int reply_fd);
static void jack_port_release (jack_engine_t *engine, jack_port_internal_t *);
static void jack_port_clear_connections (jack_engine_t *engine, jack_port_internal_t *port);
static int  jack_port_disconnect_internal (jack_engine_t *engine, jack_port_internal_t *src, 
					    jack_port_internal_t *dst, int sort_graph);

static void jack_port_registration_notify (jack_engine_t *, jack_port_id_t, int);
static int  jack_send_connection_notification (jack_engine_t *, jack_client_id_t, jack_port_id_t, jack_port_id_t, int);
static int  jack_deliver_event (jack_engine_t *, jack_client_internal_t *, jack_event_t *);
static void jack_deliver_event_to_all (jack_engine_t *engine, jack_event_t *event);
static void jack_engine_post_process (jack_engine_t *);

static int  internal_client_request (void*, jack_request_t *);

static int  jack_use_driver (jack_engine_t *engine, jack_driver_t *driver);
static int  jack_run_cycle (jack_engine_t *engine, jack_nframes_t nframes, float delayed_usecs);

static char *client_state_names[] = {
	"Not triggered",
	"Triggered",
	"Running",
	"Finished"
};

static inline int 
jack_client_is_internal (jack_client_internal_t *client)
{
	return (client->control->type == ClientInternal) || (client->control->type == ClientDriver);
}

static inline void jack_lock_graph (jack_engine_t* engine) {
	DEBUG ("acquiring graph lock");
	pthread_mutex_lock (&engine->client_lock);
}

static inline int jack_try_lock_graph (jack_engine_t *engine)
{
	DEBUG ("TRYING to acquiring graph lock");
	return pthread_mutex_trylock (&engine->client_lock);
}

static inline void jack_unlock_graph (jack_engine_t* engine) 
{
	DEBUG ("releasing graph lock");
	pthread_mutex_unlock (&engine->client_lock);
}

static inline void
jack_engine_reset_rolling_usecs (jack_engine_t *engine)
{
	memset (engine->rolling_client_usecs, 0, sizeof (engine->rolling_client_usecs));
	engine->rolling_client_usecs_index = 0;
	engine->rolling_client_usecs_cnt = 0;

	if (engine->driver) {
		engine->rolling_interval = (int) floor (JACK_ENGINE_ROLLING_INTERVAL * 1000.0f / engine->driver->period_usecs);
	} else {
		engine->rolling_interval = JACK_ENGINE_ROLLING_INTERVAL; // whatever
	}

	engine->spare_usecs = 0;
}

static inline jack_port_type_info_t *
jack_global_port_type_info (jack_engine_t *engine, jack_port_internal_t *port)
{
	/* this returns a pointer to the engine's private port type
	   information, rather than the copy that the port uses.
	   we need it for buffer allocation, because we need
	   the mutex that protects the free buffer list for
	   this port type, and that requires accessing the
	   single copy owned by the engine.
	*/

	return &engine->control->port_types[port->shared->type_info.type_id];
}

static int
make_sockets (int fd[2])
{
	struct sockaddr_un addr;
	int i;

	/* First, the master server socket */

	if ((fd[0] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
		jack_error ("cannot create server socket (%s)", strerror (errno));
		return -1;
	}

	addr.sun_family = AF_UNIX;
	for (i = 0; i < 999; i++) {
		snprintf (addr.sun_path, sizeof (addr.sun_path) - 1, "%s/jack_%d", jack_server_dir, i);
		if (access (addr.sun_path, F_OK) != 0) {
			break;
		}
	}

	if (i == 999) {
		jack_error ("all possible server socket names in use!!!");
		close (fd[0]);
		return -1;
	}

	if (bind (fd[0], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		jack_error ("cannot bind server to socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	if (listen (fd[0], 1) < 0) {
		jack_error ("cannot enable listen on server socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	/* Now the client/server event ack server socket */

	if ((fd[1] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
		jack_error ("cannot create event ACK socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	addr.sun_family = AF_UNIX;
	for (i = 0; i < 999; i++) {
		snprintf (addr.sun_path, sizeof (addr.sun_path) - 1, "%s/jack_ack_%d", jack_server_dir, i);
		if (access (addr.sun_path, F_OK) != 0) {
			break;
		}
	}

	if (i == 999) {
		jack_error ("all possible server ACK socket names in use!!!");
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	if (bind (fd[1], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		jack_error ("cannot bind server to socket (%s)", strerror (errno));
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	if (listen (fd[1], 1) < 0) {
		jack_error ("cannot enable listen on server socket (%s)", strerror (errno));
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	return 0;
}

void
jack_cleanup_files ()
{
	DIR *dir;
	struct dirent *dirent;

	/* its important that we remove all files that jackd creates
	   because otherwise subsequent attempts to start jackd will
	   believe that an instance is already running.
	*/

	if ((dir = opendir (jack_server_dir)) == NULL) {
		fprintf (stderr, "jack(%d): cannot open jack FIFO directory (%s)\n", getpid(), strerror (errno));
		return;
	}

	while ((dirent = readdir (dir)) != NULL) {
		if (strncmp (dirent->d_name, "jack-", 5) == 0 || strncmp (dirent->d_name, "jack_", 5) == 0) {
			char fullpath[PATH_MAX+1];
			snprintf (fullpath, sizeof (fullpath), "%s/%s", jack_server_dir, dirent->d_name);
			unlink (fullpath);
		} 
	}

	closedir (dir);
}

static int
jack_resize_port_segment (jack_engine_t *engine, jack_port_type_info_t *port_type,
			  jack_nframes_t buffer_size, unsigned long nports)
{
	jack_event_t event;
	char *addr;
	size_t offset;
	size_t one_buffer;
	size_t size;
	int shmid;
    int perm;

	if (port_type->buffer_scale_factor < 0) {
		one_buffer = port_type->buffer_size;
	} else {
		one_buffer = sizeof (jack_default_audio_sample_t) * port_type->buffer_scale_factor * engine->control->buffer_size;
	}

	size = nports * one_buffer;
    
#if defined(linux) 
        perm = O_RDWR|O_CREAT|O_TRUNC;
#elif defined(__APPLE__) && defined(__POWERPC__) 
        /* using O_TRUNC option does not work on Darwin */
        perm = O_RDWR|O_CREAT;
#endif

	if (port_type->shm_info.size == 0) {
        
                 snprintf (port_type->shm_info.shm_name, sizeof(port_type->shm_info.shm_name), "/jck-[%s]", port_type->type_name);

		if ((addr = jack_get_shm (port_type->shm_info.shm_name, size, 
					  perm, 0666, PROT_READ|PROT_WRITE, &shmid)) == MAP_FAILED) {
			jack_error ("cannot create new port segment of %d bytes, shm_name = %s (%s)", 
				    size, port_type->shm_info.shm_name, strerror (errno));
			return -1;
		}
		
		jack_register_shm (port_type->shm_info.shm_name, addr, shmid);
		port_type->shm_info.address = addr;

	} else {

		if ((addr = jack_resize_shm (port_type->shm_info.shm_name, size, 
					     perm, 0666, PROT_READ|PROT_WRITE)) == MAP_FAILED) {
			jack_error ("cannot resize port segment to %d bytes, shm_name = %s (%s)", 
				    size, port_type->shm_info.shm_name, strerror (errno));
			return -1;
		}
	}

	port_type->shm_info.size = size;
	port_type->shm_info.address = addr;

	pthread_mutex_lock (&port_type->buffer_lock);

	offset = 0;

	while (offset < port_type->shm_info.size) {
		jack_port_buffer_info_t *bi;

		bi = (jack_port_buffer_info_t *) malloc (sizeof (jack_port_buffer_info_t));
		bi->shm_name = port_type->shm_info.shm_name;
		bi->offset = offset;

		/* we append because we want the list to be in memory-address order */

		port_type->buffer_freelist = jack_slist_append (port_type->buffer_freelist, bi);

		offset += one_buffer;
	}

	pthread_mutex_unlock (&port_type->buffer_lock);

	/* tell everybody about it */

	event.type = NewPortType;
	strcpy (event.x.shm_name, port_type->shm_info.shm_name);
	event.y.addr = addr;

	jack_deliver_event_to_all (engine, &event);

	return 0;
}

static int
jack_set_buffer_size (jack_engine_t *engine, jack_nframes_t nframes)
{
	int i;
	jack_event_t event;

	engine->control->buffer_size = nframes;

	for (i = 0; i < engine->control->n_port_types; ++i) {
		jack_resize_port_segment (engine, &engine->control->port_types[i], nframes, engine->control->port_max);
	}

	/* convert the first chunk of the audio port buffer segment into a zero-filled area */
	
	if (engine->silent_buffer == NULL) {
		jack_port_type_info_t *port_type = &engine->control->port_types[0];

		engine->silent_buffer = (jack_port_buffer_info_t *) port_type->buffer_freelist->data;
		port_type->buffer_freelist = jack_slist_remove_link (port_type->buffer_freelist, port_type->buffer_freelist);

		memset (port_type->shm_info.address + engine->silent_buffer->offset, 0, 
			sizeof (jack_default_audio_sample_t) * nframes);
	}

	event.type = BufferSizeChange;
	jack_deliver_event_to_all (engine, &event);


	return 0;
}

static int
jack_set_sample_rate (jack_engine_t *engine, jack_nframes_t nframes)
{
	engine->control->current_time.frame_rate = nframes;
	engine->control->pending_time.frame_rate = nframes;
	return 0;
}

static JSList * 
jack_process_internal(jack_engine_t *engine, JSList *node, jack_nframes_t nframes)
{
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	
	client = (jack_client_internal_t *) node->data;
	ctl = client->control;
	
	/* internal client ("plugin") */

       if (ctl->process) {

	       DEBUG ("calling process() on an internal client");

	       ctl->state = Running;

	       /* XXX how to time out an internal client? */

	       engine->current_client = client;

	       if (ctl->process (nframes, ctl->process_arg) == 0) {
		       ctl->state = Finished;
	       } else {
		       jack_error ("internal client %s failed", client->control->name);
		       engine->process_errors++;
		       return NULL; /* will stop the loop */
	       }

       } else {
	       DEBUG ("internal client has no process() function");

	       ctl->state = Finished;
       }

       return jack_slist_next (node);
}

#if defined(linux)
static JSList * 
jack_process_external(jack_engine_t *engine, JSList *node)
{
	int status;
	char c;
	float delayed_usecs;
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	jack_time_t now, then;
	
	client = (jack_client_internal_t *) node->data;
	
	ctl = client->control;

	/* external subgraph */

	ctl->state = Triggered; // a race exists if we do this after the write(2) 
	ctl->signalled_at = jack_get_microseconds();
	ctl->awake_at = 0;
	ctl->finished_at = 0;

	engine->current_client = client;

	DEBUG ("calling process() on an external subgraph, fd==%d", client->subgraph_start_fd);

	if (write (client->subgraph_start_fd, &c, sizeof (c)) != sizeof (c)) {
		jack_error ("cannot initiate graph processing (%s)", strerror (errno));
		engine->process_errors++;
		return NULL; /* will stop the loop */
	} 

	then = jack_get_microseconds ();

	if (engine->asio_mode) {
		engine->driver->wait (engine->driver, client->subgraph_wait_fd, &status, &delayed_usecs);
	} else {
		struct pollfd pfd[1];
		int poll_timeout = (engine->control->real_time == 0 ? engine->client_timeout_msecs : engine->driver->period_usecs/1000);

		pfd[0].fd = client->subgraph_wait_fd;
		pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;

		DEBUG ("waiting on fd==%d for process() subgraph to finish", client->subgraph_wait_fd);

		if (poll (pfd, 1, poll_timeout) < 0) {
			jack_error ("poll on subgraph processing failed (%s)", strerror (errno));
			status = -1; 
		}

		if (pfd[0].revents & ~POLLIN) {
			jack_error ("subgraph starting at %s lost client", client->control->name);
			status = -2; 
		}

		if (pfd[0].revents & POLLIN) {
			status = 0;
		} else {
			jack_error ("subgraph starting at %s timed out (subgraph_wait_fd=%d, status = %d, state = %s)", 
				    client->control->name, client->subgraph_wait_fd, status, 
				    client_state_names[client->control->state]);
			status = 1;
		}
	}

	now = jack_get_microseconds ();

	if (status != 0) {
		if (engine->verbose) {
			fprintf (stderr, "at %Lu client waiting on %d took %Lu usecs, status = %d sig = %Lu "
				 "awa = %Lu fin = %Lu dur=%Lu\n",
				 now,
				 client->subgraph_wait_fd,
				 now - then,
				 status,
				 ctl->signalled_at,
				 ctl->awake_at,
				 ctl->finished_at,
				 ctl->finished_at ? ctl->finished_at - ctl->signalled_at : 0);
		}

		/* we can only consider the timeout a client error if it actually woke up.
		   its possible that the kernel scheduler screwed us up and 
		   never woke up the client in time. sigh.
		*/

		if (ctl->awake_at > 0) {
			ctl->timed_out++;
		}

		engine->process_errors++;
		return NULL; /* will stop the loop */
	} else {
		DEBUG ("reading byte from subgraph_wait_fd==%d", client->subgraph_wait_fd);

		if (read (client->subgraph_wait_fd, &c, sizeof(c)) != sizeof (c)) {
			jack_error ("pp: cannot clean up byte from graph wait fd (%s)", strerror (errno));
			client->error++;
			return NULL; /* will stop the loop */
		}
	}

	/* Move to next internal client (or end of client list) */

	while (node) {
		if (jack_client_is_internal (((jack_client_internal_t *) node->data))) {
			break;
		}
		node = jack_slist_next (node);
	}
	
	return node;
}

#elif defined(__APPLE__) && defined(__POWERPC__) 

static JSList * 
jack_process_external(jack_engine_t *engine, JSList *node)
{
        jack_client_internal_t * client = (jack_client_internal_t *) node->data;
        jack_client_control_t *ctl;
        
        client = (jack_client_internal_t *) node->data;
        ctl = client->control;
        
        engine->current_client = client;
        
        ctl->state = Triggered; // a race exists if we do this after the write(2) 
        ctl->signalled_at = jack_get_microseconds();
        ctl->awake_at = 0;
        ctl->finished_at = 0;
        
        jack_client_resume(client);
        
        return jack_slist_next (node);
}
#endif


static int
jack_engine_process (jack_engine_t *engine, jack_nframes_t nframes)
{
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	JSList *node;

	engine->process_errors = 0;

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		ctl = ((jack_client_internal_t *) node->data)->control;
		ctl->state = NotTriggered;
		ctl->nframes = nframes;
		ctl->timed_out = 0;
	}

	for (node = engine->clients; engine->process_errors == 0 && node; ) {

		client = (jack_client_internal_t *) node->data;
		
		DEBUG ("considering client %s for processing", client->control->name);

		if (!client->control->active || client->control->dead) {
			node = jack_slist_next (node);
		} else if (jack_client_is_internal (client)) {
			node = jack_process_internal (engine, node, nframes);
		} else {
			node = jack_process_external (engine, node);
		}
	}

	return engine->process_errors > 0;
}

static void 
jack_calc_cpu_load(jack_engine_t *engine)
{
	jack_time_t cycle_end = jack_get_microseconds ();
	
	/* store the execution time for later averaging */

	engine->rolling_client_usecs[engine->rolling_client_usecs_index++] = 
		cycle_end - engine->control->current_time.usecs;

	if (engine->rolling_client_usecs_index >= JACK_ENGINE_ROLLING_COUNT) {
		engine->rolling_client_usecs_index = 0;
	}

	/* every so often, recompute the current maximum use over the
	   last JACK_ENGINE_ROLLING_COUNT client iterations.
	*/

	if (++engine->rolling_client_usecs_cnt % engine->rolling_interval == 0) {
		float max_usecs = 0.0f;
		int i;

		for (i = 0; i < JACK_ENGINE_ROLLING_COUNT; i++) {
			if (engine->rolling_client_usecs[i] > max_usecs) {
				max_usecs = engine->rolling_client_usecs[i];
			}
		}

		if (max_usecs < engine->driver->period_usecs) {
			engine->spare_usecs = engine->driver->period_usecs - max_usecs;
		} else {
			engine->spare_usecs = 0;
		}

		engine->control->cpu_load = (1.0f - (engine->spare_usecs / engine->driver->period_usecs)) * 50.0f + (engine->control->cpu_load * 0.5f);

		if (engine->verbose) {
			fprintf (stderr, "load = %.4f max usecs: %.3f, spare = %.3f\n", 
				 engine->control->cpu_load, max_usecs, engine->spare_usecs);
		}
	}

}

static void
jack_remove_clients (jack_engine_t* engine)
{
	JSList *tmp, *node;
	int need_sort = FALSE;
	jack_client_internal_t *client;
	
	/* remove all dead clients */

	for (node = engine->clients; node; ) {
		
		tmp = jack_slist_next (node);
		
		client = (jack_client_internal_t *) node->data;
		
		if (client->error) {
			
			/* if we have a communication problem with the client,
			   remove it. otherwise, turn it into a zombie. the client
			   will/should realize this and will close its sockets.
			   then we'll end up back here again and will finally
			   remove the client.
			*/
			
			if (client->error >= JACK_ERROR_WITH_SOCKETS) {
				if (engine->verbose) {
					fprintf (stderr, "removing failed client %s state = %s errors = %d\n", 
						 client->control->name, client_state_names[client->control->state],
						 client->error);
				}
				jack_remove_client (engine, (jack_client_internal_t *) node->data);
			} else {
				if (engine->verbose) {
					fprintf (stderr, "zombifying failed client %s state = %s errors = %d\n", 
						 client->control->name, client_state_names[client->control->state],
						 client->error);
				}
				jack_zombify_client (engine, (jack_client_internal_t *) node->data);
				client->error = 0;
			}
			
			need_sort = TRUE;
		}
		
		node = tmp;
	}
	
	if (need_sort) {
		jack_sort_graph (engine);
	}
	
	jack_engine_reset_rolling_usecs (engine);
}

static void
jack_engine_post_process (jack_engine_t *engine)
{
	jack_client_control_t *ctl;
	jack_client_internal_t *client;
	JSList *node;
	int need_remove = FALSE;

	/* maintain the current_time.usecs and frame_rate values, since clients
	   are not permitted to set these.
	*/

	engine->control->pending_time.usecs = engine->control->current_time.usecs;
	engine->control->pending_time.frame_rate = engine->control->current_time.frame_rate;
	engine->control->current_time = engine->control->pending_time;
	
	/* find any clients that need removal due to timeouts, etc. */
		
	for (node = engine->clients; node; node = jack_slist_next (node) ) {

		client = (jack_client_internal_t *) node->data;
		ctl = client->control;

		/* this check is invalid for internal clients and external
		   clients with no process callback.
		*/

		if (!jack_client_is_internal (client) && ctl->process) {
			if (ctl->awake_at != 0 && ctl->state > NotTriggered && ctl->state != Finished && ctl->timed_out++) {
				fprintf (stderr, "client %s error: awake_at = %Lu state = %d timed_out = %d\n",
					 ctl->name,
					 ctl->awake_at,
					 ctl->state,
					 ctl->timed_out);
				client->error++;
			}
		}

		if (client->error) {
			need_remove = TRUE;
		}
	}
	
	if (need_remove) {
		jack_remove_clients (engine);
	}

	jack_calc_cpu_load (engine);
}

static int
jack_load_client (jack_engine_t *engine, jack_client_internal_t *client, const char *so_name)
{
	const char *errstr;
	char path_to_so[PATH_MAX+1];

	snprintf (path_to_so, sizeof (path_to_so), ADDON_DIR "/%s.so", so_name);
	client->handle = dlopen (path_to_so, RTLD_NOW|RTLD_GLOBAL);
	
	if (client->handle == 0) {
		if ((errstr = dlerror ()) != 0) {
			jack_error ("can't load \"%s\": %s", path_to_so, errstr);
		} else {
			jack_error ("bizarre error loading shared object %s", so_name);
		}
		return -1;
	}

	client->initialize = dlsym (client->handle, "jack_initialize");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no initialize function in shared object %s\n", so_name);
		dlclose (client->handle);
		client->handle = 0;
		return -1;
	}

	client->finish = (void (*)(void)) dlsym (client->handle, "jack_finish");
	
	if ((errstr = dlerror ()) != 0) {
		jack_error ("no finish function in in shared object %s", so_name);
		dlclose (client->handle);
		client->handle = 0;
		return -1;
	}

	return 0;
}

static void
jack_client_unload (jack_client_internal_t *client)
{
	if (client->handle) {
		if (client->finish) {
			client->finish ();
		}
		dlclose (client->handle);
	}
}

static jack_client_internal_t *
setup_client (jack_engine_t *engine, int client_fd, jack_client_connect_request_t *req, jack_client_connect_result_t *res)
{
	JSList *node;
	jack_client_internal_t *client = NULL;

	for (node = engine->clients; node; node = jack_slist_next (node)) {
	        client = (jack_client_internal_t *) node->data;

		if (strncmp(req->name, (char*)client->control->name, sizeof(req->name)) == 0) {
		        jack_error ("cannot create new client; %s already exists", client->control->name);
			return NULL;
		}
	}

	if ((client = jack_client_internal_new (engine, client_fd, req)) == NULL) {
		jack_error ("cannot create new client object");
		return 0;
	}
	
	if (engine->verbose) {
		fprintf (stderr, "new client: %s, id = %ld type %d @ %p fd = %d\n", 
			 client->control->name, client->control->id, 
			 req->type, client->control, client_fd);
	}
	
	res->protocol_v = jack_protocol_version;
	strcpy (res->client_shm_name, client->shm_name);
	strcpy (res->control_shm_name, engine->control_shm_name);
	res->control_size = engine->control_size;
	res->realtime = engine->control->real_time;
	res->realtime_priority = engine->rtpriority - 1;
    res->n_port_types = engine->control->n_port_types;
        
#if defined(__APPLE__) && defined(__POWERPC__) 	
     /* specific ressources for server/client real-time thread communication */
     res->portnum = client->portnum; 
#endif
  	
	if (jack_client_is_internal(client)) {
		
		/* set up the pointers necessary for the request system
		   to work.
		*/
		
		client->control->deliver_request = internal_client_request;
		client->control->deliver_arg = engine;
		
		/* the client is in the same address space */
		
		res->client_control = client->control;
		res->engine_control = engine->control;

	} else {
		strcpy (res->fifo_prefix, engine->fifo_prefix);
	}

	jack_lock_graph (engine);

	engine->clients = jack_slist_prepend (engine->clients, client);

	jack_engine_reset_rolling_usecs (engine);
	
	switch (client->control->type) {
	case ClientDriver:
	case ClientInternal:

		/* an internal client still needs to be able to make
		   regular JACK API calls, which need a jack_client_t
		   structure. create one here for it.
		*/

		client->control->private_client = jack_client_alloc_internal (client->control, engine->control);
		
		jack_unlock_graph (engine);

		/* call its initialization function */

		if (client->control->type == ClientInternal) {
			unsigned long i;

			/* tell it about current port types and their shared memory information */

			for (i = 0; i < engine->control->n_port_types; ++i) {
				jack_client_handle_new_port_type (client->control->private_client, 
								  engine->control->port_types[i].shm_info.shm_name,
								  engine->control->port_types[i].shm_info.size,
								  engine->control->port_types[i].shm_info.address);
			}

			if (client->initialize (client->control->private_client, req->object_data)) {
				jack_client_delete (engine, client);
				return 0;
			}
		}

		/* its good to go */
		break;

	default:
		
		if (engine->pfd_max >= engine->pfd_size) {
			engine->pfd = (struct pollfd *) realloc (engine->pfd, sizeof (struct pollfd) * engine->pfd_size + 16);
			engine->pfd_size += 16;
		}
		
		engine->pfd[engine->pfd_max].fd = client->request_fd;
		engine->pfd[engine->pfd_max].events = POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL;
		engine->pfd_max++;

		jack_unlock_graph (engine);
		break;
	}
	
	return client;
}

static jack_driver_info_t *
jack_load_driver (jack_engine_t *engine, char *so_name)
{
	const char *errstr;
	char path_to_so[PATH_MAX+1];
	jack_driver_info_t *info;

	info = (jack_driver_info_t *) calloc (1, sizeof (*info));

	snprintf (path_to_so, sizeof (path_to_so), ADDON_DIR "/jack_%s.so", so_name);
	
	info->handle = dlopen (path_to_so, RTLD_NOW|RTLD_GLOBAL);
	
	if (info->handle == NULL) {
		if ((errstr = dlerror ()) != 0) {
			jack_error ("can't load \"%s\": %s", path_to_so, errstr);
		} else {
			jack_error ("bizarre error loading driver shared object %s", path_to_so);
		}
		goto fail;
	}

	info->initialize = dlsym (info->handle, "driver_initialize");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no initialize function in shared object %s\n", path_to_so);
		goto fail;
	}

	info->finish = dlsym (info->handle, "driver_finish");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no finish function in in shared driver object %s", path_to_so);
		goto fail;
	}

	info->client_name = (char *) dlsym (info->handle, "driver_client_name");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no client name in in shared driver object %s", path_to_so);
		goto fail;
	}

	return info;

  fail:
	if (info->handle) {
		dlclose (info->handle);
	}
	free (info);
	return NULL;
	
}

void
jack_driver_unload (jack_driver_t *driver)
{
	driver->finish (driver);
	dlclose (driver->handle);
}

int
jack_engine_load_driver (jack_engine_t *engine, int argc, char *argv[])
{
	jack_client_connect_request_t req;
	jack_client_connect_result_t  res;
	jack_client_internal_t *client;
	jack_driver_t *driver;
	jack_driver_info_t *info;

	if ((info = jack_load_driver (engine, argv[0])) == NULL) {
		return -1;
	}

	req.type = ClientDriver;
	snprintf (req.name, sizeof (req.name), "%s", info->client_name);

	if ((client = setup_client (engine, -1, &req, &res)) == NULL) {
		return -1;
	}

	if ((driver = info->initialize (client->control->private_client, argc, argv)) != 0) {
		driver->handle = info->handle;
		driver->finish = info->finish;
	}

	free (info);

	if (jack_use_driver (engine, driver)) {
		jack_driver_unload (driver);
		jack_client_delete (engine, client);
		return -1;
	}

	return 0;
}

static int
handle_unload_client (jack_engine_t *engine, int client_fd, jack_client_connect_request_t *req)
{
	JSList *node;
	jack_client_connect_result_t res;

	res.status = -1;

	if (engine->verbose) {
		fprintf (stderr, "unloading client \"%s\"\n", req->name);
	}

	jack_lock_graph (engine);
	for (node = engine->clients; node; node = jack_slist_next (node)) {
		if (strcmp ((char *) ((jack_client_internal_t *) node->data)->control->name, req->name) == 0) {
			jack_remove_client (engine, (jack_client_internal_t *) node->data);
			res.status = 0;
			break;
		}
	}
	jack_unlock_graph (engine);

	return 0;
}

static int
handle_new_client (jack_engine_t *engine, int client_fd)
{
	jack_client_internal_t *client;
	jack_client_connect_request_t req;
	jack_client_connect_result_t res;
	unsigned long i;

	if (read (client_fd, &req, sizeof (req)) != sizeof (req)) {
		jack_error ("cannot read connection request from client");
		return -1;
	}

	if (!req.load) {
		return handle_unload_client (engine, client_fd, &req);
	}

	if ((client = setup_client (engine, client_fd, &req, &res)) == NULL) {
		return -1;
	}

	if (write (client->request_fd, &res, sizeof (res)) != sizeof (res)) {
		jack_error ("cannot write connection response to client");
		jack_client_delete (engine, client);
		return -1;
	}
	
	/* give external clients a chance to find out about all known port types */

	switch (client->control->type) {
	case ClientDriver:
	case ClientInternal:
		close (client_fd);
		break;

	default:
		for (i = 0; i < engine->control->n_port_types; ++i) {
			if (write (client->request_fd, &engine->control->port_types[i], sizeof (jack_port_type_info_t)) !=
			    sizeof (jack_port_type_info_t)) {
				jack_error ("cannot send port type information to new client");
				jack_client_delete (engine, client);
			}
		}
		break;
	}

	return 0;
}

static int
handle_client_ack_connection (jack_engine_t *engine, int client_fd)

{
	jack_client_internal_t *client;
	jack_client_connect_ack_request_t req;
	jack_client_connect_ack_result_t res;

	if (read (client_fd, &req, sizeof (req)) != sizeof (req)) {
		jack_error ("cannot read ACK connection request from client");
		return -1;
	}

	if ((client = jack_client_internal_by_id (engine, req.client_id)) == NULL) {
		jack_error ("unknown client ID in ACK connection request");
		return -1;
	}

	client->event_fd = client_fd;

	res.status = 0;

	if (write (client->event_fd, &res, sizeof (res)) != sizeof (res)) {
		jack_error ("cannot write ACK connection response to client");
		return -1;
	}

	return 0;
}

#ifdef USE_CAPABILITIES

static int check_capabilities (jack_engine_t *engine)

{
	cap_t caps = cap_init();
	cap_flag_value_t cap;
	pid_t pid;
	int have_all_caps = 1;

	if (caps == NULL) {
		if (engine->verbose) {
			fprintf (stderr, "check: could not allocate capability working storage\n");
		}
		return 0;
	}
	pid = getpid ();
	cap_clear (caps);
	if (capgetp (pid, caps)) {
		if (engine->verbose) {
			fprintf (stderr, "check: could not get capabilities for process %d\n", pid);
		}
		return 0;
	}
	/* check that we are able to give capabilites to other processes */
	cap_get_flag(caps, CAP_SETPCAP, CAP_EFFECTIVE, &cap);
	if (cap == CAP_CLEAR) {
		have_all_caps = 0;
		goto done;
	}
	/* check that we have the capabilities we want to transfer */
	cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &cap);
	if (cap == CAP_CLEAR) {
		have_all_caps = 0;
		goto done;
	}
	cap_get_flag(caps, CAP_SYS_RESOURCE, CAP_EFFECTIVE, &cap);
	if (cap == CAP_CLEAR) {
		have_all_caps = 0;
		goto done;
	}
	cap_get_flag(caps, CAP_IPC_LOCK, CAP_EFFECTIVE, &cap);
	if (cap == CAP_CLEAR) {
		have_all_caps = 0;
		goto done;
	}
  done:
	cap_free (caps);
	return have_all_caps;
}


static int give_capabilities (jack_engine_t *engine, pid_t pid)

{
	cap_t caps = cap_init();
	const unsigned caps_size = 3;
	cap_value_t cap_list[] = { CAP_SYS_NICE, CAP_SYS_RESOURCE, CAP_IPC_LOCK};

	if (caps == NULL) {
		if (engine->verbose) {
			fprintf (stderr, "give: could not allocate capability working storage\n");
		}
		return -1;
	}
	cap_clear(caps);
	if (capgetp (pid, caps)) {
		if (engine->verbose) {
			fprintf (stderr, "give: could not get current capabilities for process %d\n", pid);
		}
		cap_clear(caps);
	}
	cap_set_flag(caps, CAP_EFFECTIVE, caps_size, cap_list , CAP_SET);
	cap_set_flag(caps, CAP_INHERITABLE, caps_size, cap_list , CAP_SET);
	cap_set_flag(caps, CAP_PERMITTED, caps_size, cap_list , CAP_SET);
	if (capsetp (pid, caps)) {
		cap_free (caps);
		return -1;
	}
	cap_free (caps);
	return 0;
}


static int
jack_set_client_capabilities (jack_engine_t *engine, jack_client_id_t id)

{
	JSList *node;
	int ret = -1;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = jack_slist_next (node)) {

		jack_client_internal_t *client = (jack_client_internal_t *) node->data;

		if (client->control->id == id) {

			/* before sending this request the client has already checked
			   that the engine has realtime capabilities, that it is running
			   realtime and that the pid is defined
			*/
			ret = give_capabilities (engine, client->control->pid);
			if (ret) {
				jack_error ("could not give capabilities to process %d\n",
					    client->control->pid);
			} else {
				if (engine->verbose) {
					fprintf (stderr, "gave capabilities to process %d\n",
						 client->control->pid);
				}
			}
		}
	}

	jack_unlock_graph (engine);

	return ret;
}	

#endif


static int
jack_client_activate (jack_engine_t *engine, jack_client_id_t id)

{
	jack_client_internal_t *client;
	JSList *node;
	int ret = -1;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = jack_slist_next (node)) {

		if (((jack_client_internal_t *) node->data)->control->id == id) {
		       
			client = (jack_client_internal_t *) node->data;
			client->control->active = TRUE;

			/* we call this to make sure the
			   FIFO is built+ready by the time
			   the client needs it. we don't
			   care about the return value at
			   this point.  
			*/

			jack_get_fifo_fd (engine, ++engine->external_client_cnt);
			jack_sort_graph (engine);

			ret = 0;
			break;
		}
	}

	jack_unlock_graph (engine);
	return ret;
}	

static int
jack_client_do_deactivate (jack_engine_t *engine, jack_client_internal_t *client, int sort_graph)

{
	/* called must hold engine->client_lock and must have checked for and/or
	   cleared all connections held by client.
	*/
	
	client->control->active = FALSE;

	if (!jack_client_is_internal (client) && engine->external_client_cnt > 0) {	
		engine->external_client_cnt--;
	}

	if (sort_graph) {
		jack_sort_graph (engine);
	}
	return 0;
}

static void
jack_client_disconnect (jack_engine_t *engine, jack_client_internal_t *client)

{
	JSList *node;
	jack_port_internal_t *port;

	/* call tree **** MUST HOLD *** engine->client_lock */

	for (node = client->ports; node; node = jack_slist_next (node)) {
		port = (jack_port_internal_t *) node->data;
		jack_port_clear_connections (engine, port);
		jack_port_release (engine, port);
	}

	jack_slist_free (client->ports);
	jack_slist_free (client->fed_by);
	client->fed_by = 0;
	client->ports = 0;
}			

static int
jack_client_deactivate (jack_engine_t *engine, jack_client_id_t id)

{
	JSList *node;
	int ret = -1;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = jack_slist_next (node)) {

		jack_client_internal_t *client = (jack_client_internal_t *) node->data;

		if (client->control->id == id) {
		        
	        	JSList *portnode;
			jack_port_internal_t *port;

			if (client == engine->timebase_client) {
				engine->timebase_client = 0;
				engine->control->current_time.frame = 0;
				engine->control->pending_time.frame = 0;
				engine->control->current_time.transport_state = JackTransportStopped;
				engine->control->pending_time.transport_state = JackTransportStopped;
				engine->control->current_time.valid = JackTransportState|JackTransportPosition;
				engine->control->pending_time.valid = JackTransportState|JackTransportPosition;
			}
			
			for (portnode = client->ports; portnode; portnode = jack_slist_next (portnode)) {
				port = (jack_port_internal_t *) portnode->data;
				jack_port_clear_connections (engine, port);
 			}

			ret = jack_client_do_deactivate (engine, node->data, TRUE);
			break;
		}
	}

	jack_unlock_graph (engine);

	return ret;
}	

static int
jack_set_timebase (jack_engine_t *engine, jack_client_id_t client)
{
	int ret = -1;

	jack_lock_graph (engine);

	if ((engine->timebase_client = jack_client_internal_by_id (engine, client)) != 0) {
		ret = 0;
	}

	jack_unlock_graph (engine);

	return ret;
}

static int
handle_client_socket_error (jack_engine_t *engine, int fd)
{
	jack_client_internal_t *client = 0;
	JSList *node;

#ifndef DEFER_CLIENT_REMOVE_TO_AUDIO_THREAD

        jack_lock_graph (engine);

        for (node = engine->clients; node; node = jack_slist_next (node)) {

                if (jack_client_is_internal((jack_client_internal_t *) node->data)) {
                        continue;
                }

                if (((jack_client_internal_t *) node->data)->request_fd == fd) {
                        client = (jack_client_internal_t *) node->data;
                        break;
                }
        }

        if (client) {
		if (engine->verbose) {
			fprintf (stderr, "removing failed client %s state = %s errors = %d\n", 
				 client->control->name, client_state_names[client->control->state],
				 client->error);
		}
		jack_remove_client(engine, client);
		jack_sort_graph (engine);
	}

        jack_unlock_graph (engine);

#else

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = jack_slist_next (node)) {

		if (jack_client_is_internal((jack_client_internal_t *) node->data)) {
			continue;
		}

		if (((jack_client_internal_t *) node->data)->request_fd == fd) {
			client = (jack_client_internal_t *) node->data;
			if (client->error < JACK_ERROR_WITH_SOCKETS) {
				client->error += JACK_ERROR_WITH_SOCKETS;
			}
			break;
		}
	}

	jack_unlock_graph (engine);
#endif

	return 0;
}

static void
do_request (jack_engine_t *engine, jack_request_t *req, int *reply_fd)
{
	pthread_mutex_lock (&engine->request_lock);

	DEBUG ("got a request of type %d", req->type);

	switch (req->type) {
	case RegisterPort:
		req->status = jack_port_do_register (engine, req);
		break;

	case UnRegisterPort:
		req->status = jack_port_do_unregister (engine, req);
		break;

	case ConnectPorts:
		req->status = jack_port_do_connect (engine, req->x.connect.source_port, req->x.connect.destination_port);
		break;

	case DisconnectPort:
		req->status = jack_port_do_disconnect_all (engine, req->x.port_info.port_id);
		break;

	case DisconnectPorts:
		req->status = jack_port_do_disconnect (engine, req->x.connect.source_port, req->x.connect.destination_port);
		break;

	case ActivateClient:
		req->status = jack_client_activate (engine, req->x.client_id);
		break;

	case DeactivateClient:
		req->status = jack_client_deactivate (engine, req->x.client_id);
		break;

	case SetTimeBaseClient:
		req->status = jack_set_timebase (engine, req->x.client_id);
		break;

#ifdef USE_CAPABILITIES
	case SetClientCapabilities:
		req->status = jack_set_client_capabilities (engine, req->x.client_id);
		break;
#endif
		
	case GetPortConnections:
	case GetPortNConnections:
		if ((req->status = jack_do_get_port_connections (engine, req, *reply_fd)) == 0) {
			/* we have already replied, don't do it again */
			*reply_fd = -1;
		}
		break;

	default:
		/* some requests are handled entirely on the client side,
		   by adjusting the shared memory area(s)
		*/
		break;
	}

	pthread_mutex_unlock (&engine->request_lock);

	DEBUG ("status of request: %d", req->status);
}

static int
internal_client_request (void* ptr, jack_request_t *request)
{
	do_request ((jack_engine_t*) ptr, request, 0);
	return request->status;
}

static int
handle_external_client_request (jack_engine_t *engine, int fd)
{
	jack_request_t req;
	jack_client_internal_t *client = 0;
	int reply_fd;
	JSList *node;
	ssize_t r;

	DEBUG ("HIT: before lock");
	
	jack_lock_graph (engine);

	DEBUG ("HIT: before for");
	
	for (node = engine->clients; node; node = jack_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->request_fd == fd) {
			DEBUG ("HIT: in for");
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}
	DEBUG ("HIT: after for");

	jack_unlock_graph (engine);

	if (client == NULL) {
		jack_error ("client input on unknown fd %d!", fd);
		return -1;
	}

	if ((r = read (client->request_fd, &req, sizeof (req))) < (ssize_t) sizeof (req)) {
		jack_error ("cannot read request from client (%d/%d/%s)", r, sizeof(req), strerror (errno));
		client->error++;
		return -1;
	}

	reply_fd = client->request_fd;
	
	do_request (engine, &req, &reply_fd);
	
	if (reply_fd >= 0) {
		DEBUG ("replying to client");
		if (write (reply_fd, &req, sizeof (req)) < (ssize_t) sizeof (req)) {
			jack_error ("cannot write request result to client");
			return -1;
		}
	} else {
		DEBUG ("*not* replying to client");
        }

	return 0;
}

static void *
jack_server_thread (void *arg)

{
	jack_engine_t *engine = (jack_engine_t *) arg;
	struct sockaddr_un client_addr;
	socklen_t client_addrlen;
	struct pollfd *pfd;
	int client_socket;
	int done = 0;
	int i;
	int max;
	
	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	engine->pfd[0].fd = engine->fds[0];
	engine->pfd[0].events = POLLIN|POLLERR;
	engine->pfd[1].fd = engine->fds[1];
	engine->pfd[1].events = POLLIN|POLLERR;
	engine->pfd_max = 2;


	while (!done) {
		DEBUG ("start while");

		/* XXX race here with new external clients
		   causing engine->pfd to be reallocated.
		   I don't know how to solve this
		   short of copying the entire
		   contents of the pfd struct. Ick.
		*/
	
		max = engine->pfd_max;
		pfd = engine->pfd;
	
		if (poll (pfd, max, 10000) < 0) {
			if (errno == EINTR) {
				continue;
			}
			jack_error ("poll failed (%s)", strerror (errno));
			break;
		}
			
		/* check each client socket before handling other request*/

		for (i = 2; i < max; i++) {
			if (pfd[i].fd < 0) {
				continue;
			}

			if (pfd[i].revents & ~POLLIN) {
				handle_client_socket_error (engine, pfd[i].fd);
			} else if (pfd[i].revents & POLLIN) {
				if (handle_external_client_request (engine, pfd[i].fd)) {
					jack_error ("could not handle external client request");
				#if defined(__APPLE__) && defined(__POWERPC__) 
                                    /* poll is implemented using select (see the fakepool code). When the socket is closed
                                    select does not return any error, POLLIN is true and the next read will return 0 bytes. 
                                    This behaviour is diffrent from the Linux poll behaviour. Thus we use this condition 
                                    as a socket error and remove the client.
                                    */
                                    handle_client_socket_error(engine, pfd[i].fd);
                                #endif
                                                }
			}
		}
		
		/* check the master server socket */

		if (pfd[0].revents & POLLERR) {
			jack_error ("error on server socket");
			break;
		}
	

		if (pfd[0].revents & POLLIN) {
			DEBUG ("pfd[0].revents & POLLIN");

			memset (&client_addr, 0, sizeof (client_addr));
			client_addrlen = sizeof (client_addr);

			if ((client_socket = accept (engine->fds[0], (struct sockaddr *) &client_addr, &client_addrlen)) < 0) {
				jack_error ("cannot accept new connection (%s)", strerror (errno));
			} else if (handle_new_client (engine, client_socket) < 0) {
				jack_error ("cannot complete new client connection process");
				close (client_socket);
			}
		}

		/* check the ACK server socket */

		if (pfd[1].revents & POLLERR) {
			jack_error ("error on server ACK socket");
			break;
		}

		if (pfd[1].revents & POLLIN) {
			DEBUG ("pfd[1].revents & POLLIN");

			memset (&client_addr, 0, sizeof (client_addr));
			client_addrlen = sizeof (client_addr);

			if ((client_socket = accept (engine->fds[1], (struct sockaddr *) &client_addr, &client_addrlen)) < 0) {
				jack_error ("cannot accept new ACK connection (%s)", strerror (errno));
			} else if (handle_client_ack_connection (engine, client_socket)) {
				jack_error ("cannot complete client ACK connection process");
				close (client_socket);
			}
		}

		
	}

	return 0;
}

jack_engine_t *
jack_engine_new (int realtime, int rtpriority, int verbose, int client_timeout)
{
	jack_engine_t *engine;
	void *addr;
	unsigned int i;
	int shmid;
    int perm;
        
#ifdef USE_CAPABILITIES
	uid_t uid = getuid ();
	uid_t euid = geteuid ();
#endif
	jack_init_time ();

	engine = (jack_engine_t *) malloc (sizeof (jack_engine_t));

	engine->driver = 0;
	engine->set_sample_rate = jack_set_sample_rate;
	engine->set_buffer_size = jack_set_buffer_size;
        engine->run_cycle = jack_run_cycle;
	engine->client_timeout_msecs = client_timeout;

	engine->next_client_id = 1;
	engine->timebase_client = 0;
	engine->port_max = 128;
	engine->rtpriority = rtpriority;
	engine->silent_buffer = 0;
	engine->verbose = verbose;
	engine->asio_mode = FALSE;

	jack_engine_reset_rolling_usecs (engine);

	pthread_mutex_init (&engine->client_lock, 0);
	pthread_mutex_init (&engine->port_lock, 0);
	pthread_mutex_init (&engine->request_lock, 0);

	engine->clients = 0;

	engine->pfd_size = 16;
	engine->pfd_max = 0;
	engine->pfd = (struct pollfd *) malloc (sizeof (struct pollfd) * engine->pfd_size);

	engine->fifo_size = 16;
	engine->fifo = (int *) malloc (sizeof (int) * engine->fifo_size);
	for (i = 0; i < engine->fifo_size; i++) {
		engine->fifo[i] = -1;
	}

	engine->external_client_cnt = 0;

	srandom (time ((time_t *) 0));

	snprintf (engine->control_shm_name, sizeof (engine->control_shm_name), "/jack-engine");
	engine->control_size = sizeof (jack_control_t) + (sizeof (jack_port_shared_t) * engine->port_max);

	if (jack_initialize_shm (engine)) {
		return 0;
	}
        
#if defined(linux) 
        perm = O_RDWR|O_CREAT|O_TRUNC;
#elif defined(__APPLE__) && defined(__POWERPC__) 
        /* using O_TRUNC option does not work on Darwin */
        perm = O_RDWR|O_CREAT;
#endif

	if ((addr = jack_get_shm (engine->control_shm_name, engine->control_size, 
				  perm, 0644, PROT_READ|PROT_WRITE, &shmid)) == MAP_FAILED) {
		jack_error ("cannot create engine control shared memory segment (%s)", strerror (errno));
		return 0;
	}

	jack_register_shm (engine->control_shm_name, addr, shmid);
	
	engine->control = (jack_control_t *) addr;
	engine->control->engine = engine;

	/* setup port type information from builtins. buffer space is allocated
	   whenever we call jack_set_buffer_size()
	*/

	for (i = 0; jack_builtin_port_types[i].type_name[0]; ++i) {

		memcpy (&engine->control->port_types[i], &jack_builtin_port_types[i], sizeof (jack_port_type_info_t));
		
		// set offset into port_types array

		engine->control->port_types[i].type_id = i;

		// be sure to initialize mutex correctly

		pthread_mutex_init (&engine->control->port_types[i].buffer_lock, NULL);

		// indicate no shared memory allocation for this port type

		engine->control->port_types[i].shm_info.size = 0;
	}

	engine->control->n_port_types = i;

	/* Mark all ports as available */

	for (i = 0; i < engine->port_max; i++) {
		engine->control->ports[i].in_use = 0;
		engine->control->ports[i].id = i;
	}

	/* allocate internal port structures so that we can keep
	   track of port connections.
	*/

	engine->internal_ports = (jack_port_internal_t *) malloc (sizeof (jack_port_internal_t) * engine->port_max);

	for (i = 0; i < engine->port_max; i++) {
		engine->internal_ports[i].connections = 0;
	}

	if (make_sockets (engine->fds) < 0) {
		jack_error ("cannot create server sockets");
		return 0;
	}

	engine->control->port_max = engine->port_max;
	engine->control->real_time = realtime;
	engine->control->client_priority = engine->rtpriority - 1;
	engine->control->cpu_load = 0;
 
	engine->control->buffer_size = 0;
	engine->control->current_time.frame_rate = 0;
	engine->control->current_time.frame = 0;
	engine->control->pending_time.frame_rate = 0;
	engine->control->pending_time.frame = 0;
	engine->control->internal = 0;

	engine->control->has_capabilities = 0;
        
#if defined(__APPLE__) && defined(__POWERPC__) 
        /* specific ressources for server/client real-time thread communication */
	engine->servertask = mach_task_self();
	if (task_get_bootstrap_port(engine->servertask, &engine->bp)){
            jack_error("Jackd: Can't find bootstrap mach port");
            return 0;
        }
        engine->portnum = 0;
#endif
        
        
#ifdef USE_CAPABILITIES
	if (uid == 0 || euid == 0) {
		if (engine->verbose) {
			fprintf (stderr, "running with uid=%d and euid=%d, will not try to use capabilites\n",
				 uid, euid);
		}
	} else {
		/* only try to use capabilities if we are not running as root */
		engine->control->has_capabilities = check_capabilities (engine);
		if (engine->control->has_capabilities == 0) {
			if (engine->verbose) {
				fprintf (stderr, "required capabilities not available\n");
			}
		}
		if (engine->verbose) {
			size_t size;
			cap_t cap = cap_init();
			capgetp(0, cap);
			fprintf (stderr, "capabilities: %s\n", cap_to_text(cap, &size));
		}
	}
#endif
	engine->control->engine_ok = 1;
	snprintf (engine->fifo_prefix, sizeof (engine->fifo_prefix), "%s/jack-ack-fifo-%d", jack_server_dir, getpid());

	(void) jack_get_fifo_fd (engine, 0);

	pthread_create (&engine->server_thread, 0, &jack_server_thread, engine);
	pthread_detach (engine->server_thread);

	return engine;
}

static int
jack_become_real_time (pthread_t thread, int priority)

{
	struct sched_param rtparam;
	int x;

	memset (&rtparam, 0, sizeof (rtparam));
	rtparam.sched_priority = priority;

	if ((x = pthread_setschedparam (thread, SCHED_FIFO, &rtparam)) != 0) {
		jack_error ("cannot set thread to real-time priority (FIFO/%d) (%d: %s)", rtparam.sched_priority, x, strerror (errno));
		return -1;
	}
        
#if defined(linux) 
        if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0) {
	    jack_error ("cannot lock down memory for RT thread (%s)", strerror (errno));
	    return -1;
	}
#elif defined(__APPLE__) && defined(__POWERPC__) 
        // To be implemented
#endif
	return 0;
}

#ifdef HAVE_ON_EXIT
static void
cancel_cleanup (int status, void *arg)

{
	jack_engine_t *engine = (jack_engine_t *) arg;
	engine->control->engine_ok = 0;
	engine->driver->stop (engine->driver);
	engine->driver->finish (engine->driver);
}
#else
#ifdef HAVE_ATEXIT
jack_engine_t *global_engine;

static void
cancel_cleanup (void)

{
	jack_engine_t *engine = global_engine;
	engine->driver->stop (engine->driver);
	engine->driver->finish (engine->driver);
}
#else
#error "Don't know how to make an exit handler"
#endif /* HAVE_ATEXIT */
#endif /* HAVE_ON_EXIT */

static void *
watchdog_thread (void *arg)
{
	jack_engine_t *engine = (jack_engine_t *) arg;
	int watchdog_priority = (engine->rtpriority) > 89 ? 99 : engine->rtpriority + 10;

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	if (jack_become_real_time (pthread_self(), watchdog_priority)) {
		return 0;
	}

	engine->watchdog_check = 0;

	while (1) {
		usleep (5000000);
		if (engine->watchdog_check == 0) {
			jack_error ("jackd watchdog: timeout - killing jackd");
			/* kill the process group of the current client */
			kill (-engine->current_client->control->pid, SIGKILL);
			/* kill our process group */
			kill (-getpgrp(), SIGKILL);
			/*NOTREACHED*/
			exit (1);
		}
		engine->watchdog_check = 0;
	}
}

static int
jack_start_watchdog (jack_engine_t *engine)
{
	pthread_t watchdog;

	if (pthread_create (&watchdog, 0, watchdog_thread, engine)) {
		jack_error ("cannot start watchdog thread");
		return -1;
	}
	pthread_detach (watchdog);
	return 0;
}

static void
jack_engine_notify_clients_about_delay (jack_engine_t *engine)
{
	JSList *node;
	jack_event_t event;

	event.type = XRun;

	jack_lock_graph (engine);
	for (node = engine->clients; node; node = jack_slist_next (node)) {
		jack_deliver_event (engine, (jack_client_internal_t *) node->data, &event);
	}
	jack_unlock_graph (engine);
}

static inline void
jack_inc_frame_time (jack_engine_t *engine, jack_nframes_t amount)
{
	jack_frame_timer_t *time = &engine->control->frame_timer;
	
	// atomic_inc (&time->guard1, 1);
	// really need a memory barrier here

	time->guard1++;
	time->frames += amount;
	time->stamp = engine->driver->last_wait_ust;

	// atomic_inc (&time->guard2, 1);
	// might need a memory barrier here
	time->guard2++;
}

static int
jack_run_cycle (jack_engine_t *engine, jack_nframes_t nframes, float delayed_usecs)
{
	int restart = 0;
	jack_driver_t* driver = engine->driver;
	int ret = -1;
	static int consecutive_excessive_delays = 0;

	engine->watchdog_check = 1;

#define WORK_SCALE 1.0f

	if (engine->control->real_time && engine->spare_usecs && ((WORK_SCALE * engine->spare_usecs) <= delayed_usecs)) {
		
		fprintf (stderr, "delay of %.3f usecs exceeds estimated spare time of %.3f; restart ...\n",
			 delayed_usecs, WORK_SCALE * engine->spare_usecs);
		
		if (++consecutive_excessive_delays > 10) {
			jack_error ("too many consecutive interrupt delays ... engine pausing");
			return -1; /* will exit the thread loop */
		}
		
		if (driver->stop (driver)) {
			jack_error ("cannot stop current driver");
			return -1; /* will exit the thread loop */
		}
		
		jack_engine_notify_clients_about_delay (engine);
		
		if (driver->start (driver)) {
			jack_error ("cannot restart current driver after delay");
			return -1; /* will exit the thread loop */
		}

		return 0;

	} else {
		consecutive_excessive_delays = 0;
	}

	jack_inc_frame_time (engine, nframes);

	if (jack_try_lock_graph (engine)) {
		/* engine can't run. just throw away an entire cycle */
		driver->null_cycle (driver, nframes);
		return 0;
	}
	
	if (driver->read (driver, nframes)) {
		goto unlock;
	}

	if (jack_engine_process (engine, nframes)) {
		driver->stop (driver);
		restart = 1;
	} else {
		if (driver->write (driver, nframes)) {
			goto unlock;
		}
	}

	jack_engine_post_process (engine);
	ret = 0;

  unlock:
	jack_unlock_graph (engine);
	
	if (restart) {
		driver->start (driver);
	}
	
	return ret;
}

static void *
jack_main_thread (void *arg)
{
	jack_engine_t *engine = (jack_engine_t *) arg;
	jack_driver_t *driver = engine->driver;
	int wait_status;
	jack_nframes_t nframes;
	float delayed_usecs;

	if (engine->control->real_time) {

		if (jack_start_watchdog (engine)) {
			pthread_exit (0);
		}

		if (jack_become_real_time (pthread_self(), engine->rtpriority)) {
			engine->control->real_time = 0;
		}
	}

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	engine->watchdog_check = 1;

	while (1) {
	
		if ((nframes = driver->wait (driver, -1, &wait_status, &delayed_usecs)) == 0) {
			/* the driver detected an xrun, and restarted */
			jack_engine_notify_clients_about_delay (engine);
			continue;
		}

		if (wait_status == 0) {
			if (jack_run_cycle (engine, nframes, delayed_usecs)) {
				jack_error ("cycle execution failure, exiting");
				break;
			}
		} else if (wait_status < 0) {
			break;
 		} else {
			/* driver restarted, just continue */
		}
	}

	pthread_exit (0);
        
    return 0;
}

int
jack_run (jack_engine_t *engine)
{
	
#ifdef HAVE_ON_EXIT
	on_exit (cancel_cleanup, engine);
#else
#ifdef HAVE_ATEXIT
	global_engine = engine;
	atexit (cancel_cleanup);
#else
#error "Don't know how to install an exit handler"
#endif /* HAVE_ATEXIT */
#endif /* HAVE_ON_EXIT */

        if (engine->driver == NULL) {
                jack_error ("engine driver not set; cannot start");
                return -1;
        }

        if (engine->driver->start (engine->driver)) {
                jack_error ("cannot start driver");
                return -1;
        }
        
#if defined(linux)
        return pthread_create (&engine->main_thread, 0, jack_main_thread, engine);
#elif defined(__APPLE__) && defined(__POWERPC__) 
        return 0;
#endif

}


#if defined(linux)
int
jack_wait (jack_engine_t *engine)
{
	void *ret = 0;
	int err;

	if ((err = pthread_join (engine->main_thread, &ret)) != 0)  {
		switch (err) {
		case EINVAL:
			jack_error ("cannot join with audio thread (thread detached, or another thread is waiting)");
			break;
		case ESRCH:
			jack_error ("cannot join with audio thread (thread no longer exists)");
			break;
		case EDEADLK:
			jack_error ("programming error: jack_wait() called by audio thread");
			break;
		default:
			jack_error ("cannot join with audio thread (%s)", strerror (errno));
		}
	}
	return (int) ((intptr_t)ret);
}

#elif defined(__APPLE__) && defined(__POWERPC__) 

int
jack_wait (jack_engine_t *engine)
{
        while(1) sleep(1);
}

#endif

int 
jack_engine_delete (jack_engine_t *engine)
{
	if (engine) {

    #if defined(linux)
            return pthread_cancel (engine->main_thread);
    #elif defined(__APPLE__) && defined(__POWERPC__) 
            /* the jack_run_cycle function is directly called from the CoreAudo audio callback */
            return 0;
    #endif
	}

	return 0;
}

static jack_client_internal_t *
jack_client_internal_new (jack_engine_t *engine, int fd, jack_client_connect_request_t *req)
{
	jack_client_internal_t *client;
	shm_name_t shm_name;
	int shmid;
    int perm;
	void *addr = 0;

	switch (req->type) {
	case ClientInternal:
	case ClientDriver:
		break;

	case ClientExternal:
                
        #if defined(linux) 
                perm = O_RDWR|O_CREAT|O_TRUNC;
        #elif defined(__APPLE__) && defined(__POWERPC__) 
                /* using O_TRUNC option does not work on Darwin */
                perm = O_RDWR|O_CREAT;
        #endif
                snprintf (shm_name, sizeof (shm_name), "/jack-c-%s", req->name);
                
                if ((addr = jack_get_shm (shm_name, sizeof (jack_client_control_t), 
                                          perm, 0666, PROT_READ|PROT_WRITE, &shmid)) == MAP_FAILED) {
                        jack_error ("cannot create client control block for %s", req->name);
                        return 0;
                }
                jack_register_shm (shm_name, addr, shmid);
                break;
        }
                
	client = (jack_client_internal_t *) malloc (sizeof (jack_client_internal_t));

	client->request_fd = fd;
	client->event_fd = -1;
	client->ports = 0;
	client->fed_by = 0;
	client->execution_order = UINT_MAX;
	client->next_client = NULL;
	client->handle = NULL;
	client->finish = NULL;
	client->error = 0;

	if (req->type != ClientExternal) {
		
		client->control = (jack_client_control_t *) malloc (sizeof (jack_client_control_t));		

	} else {
		strcpy (client->shm_name, shm_name);
		client->control = (jack_client_control_t *) addr;
	}

	client->control->type = req->type;
	client->control->active = 0;
	client->control->dead = FALSE;
	client->control->timed_out = 0;
	client->control->id = engine->next_client_id++;
	strcpy ((char *) client->control->name, req->name);
	client->subgraph_start_fd = -1;
	client->subgraph_wait_fd = -1;

	client->control->process = NULL;
	client->control->process_arg = NULL;
	client->control->bufsize = NULL;
	client->control->bufsize_arg = NULL;
	client->control->srate = NULL;
	client->control->srate_arg = NULL;
	client->control->xrun = NULL;
	client->control->xrun_arg = NULL;
	client->control->port_register = NULL;
	client->control->port_register_arg = NULL;
	client->control->graph_order = NULL;
	client->control->graph_order_arg = NULL;
        
#if defined(__APPLE__) && defined(__POWERPC__) 
        /* specific ressources for server/client real-time thread communication */
        allocate_mach_serverport(engine, client);
        client->running = FALSE;
#endif
	if (req->type == ClientInternal) {
		if (jack_load_client (engine, client, req->object_path)) {
			jack_error ("cannot dynamically load client from \"%s\"", req->object_path);
			jack_client_delete (engine, client);
			return 0;
		}
	}

	return client;
}

static void
jack_port_clear_connections (jack_engine_t *engine, jack_port_internal_t *port)
{
	JSList *node, *next;

	for (node = port->connections; node; ) {
		next = jack_slist_next (node);
		jack_port_disconnect_internal (engine, 
						((jack_connection_internal_t *) node->data)->source,
						((jack_connection_internal_t *) node->data)->destination, 
						FALSE);
		node = next;
	}

	jack_slist_free (port->connections);
	port->connections = 0;
}

static void
jack_zombify_client (jack_engine_t *engine, jack_client_internal_t *client)
{
	if (engine->verbose) {
		fprintf (stderr, "*&*&*&*&** senor %s - you are a ZOMBIE\n", client->control->name);
	}

	/* caller must hold the client_lock */

	/* this stops jack_deliver_event() from doing anything */

	client->control->dead = TRUE;
	
	if (client == engine->timebase_client) {
		engine->timebase_client = 0;
		engine->control->current_time.frame = 0;
		engine->control->pending_time.frame = 0;
		engine->control->current_time.transport_state = JackTransportStopped;
		engine->control->pending_time.transport_state = JackTransportStopped;
		engine->control->current_time.valid = JackTransportState|JackTransportPosition;
		engine->control->pending_time.valid = JackTransportState|JackTransportPosition;
	}

	jack_client_disconnect (engine, client);
	jack_client_do_deactivate (engine, client, FALSE);
}

static void
jack_remove_client (jack_engine_t *engine, jack_client_internal_t *client)
{
	unsigned int i;
	JSList *node;

	/* caller must hold the client_lock */

	if (engine->verbose) {
		fprintf (stderr, "adios senor %s\n", client->control->name);
	}

	/* if its not already a zombie, make it so */

	if (!client->control->dead) {
		jack_zombify_client (engine, client);
	}

	/* try to force the server thread to return from poll */
	
	close (client->event_fd);
	close (client->request_fd);

	if (client->control->type == ClientExternal) {

		/* rearrange the pollfd array so that things work right the 
		   next time we go into poll(2).
		*/
		
		for (i = 0; i < engine->pfd_max; i++) {
			if (engine->pfd[i].fd == client->request_fd) {
				if (i+1 < engine->pfd_max) {
					memmove (&engine->pfd[i], &engine->pfd[i+1], sizeof (struct pollfd) * (engine->pfd_max - i));
				}
				engine->pfd_max--;
			}
		}
	}

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->control->id == client->control->id) {
			engine->clients = jack_slist_remove_link (engine->clients, node);
			jack_slist_free_1 (node);
			break;
		}
	}
	
	jack_client_delete (engine, client);
}


static void
jack_client_delete (jack_engine_t *engine, jack_client_internal_t *client)
{
	if (jack_client_is_internal (client)) {
		jack_client_unload (client);
		free ((char *) client->control);
	} else {
		jack_destroy_shm (client->shm_name);
		jack_release_shm ((char*)client->control, sizeof (jack_client_control_t));
	}

	free (client);
}

jack_client_internal_t *
jack_client_by_name (jack_engine_t *engine, const char *name)

{
	jack_client_internal_t *client = NULL;
	JSList *node;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		if (strcmp ((const char *) ((jack_client_internal_t *) node->data)->control->name, name) == 0) {
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}

	jack_unlock_graph (engine);
	return client;
}

jack_client_internal_t *
jack_client_internal_by_id (jack_engine_t *engine, jack_client_id_t id)

{
	jack_client_internal_t *client = NULL;
	JSList *node;

	/* call tree ***MUST HOLD*** the graph lock */

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->control->id == id) {
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}

	return client;
}

static void
jack_deliver_event_to_all (jack_engine_t *engine, jack_event_t *event)
{
	JSList *node;

	jack_lock_graph (engine);
	for (node = engine->clients; node; node = jack_slist_next (node)) {
		jack_deliver_event (engine, (jack_client_internal_t *) node->data, event);
	}
	jack_unlock_graph (engine);
}

static int
jack_deliver_event (jack_engine_t *engine, jack_client_internal_t *client, jack_event_t *event)
{
	char status;

	/* caller must hold the graph lock */

	DEBUG ("delivering event (type %d)", event->type);

	if (client->control->dead) {
		return 0;
	}

	if (jack_client_is_internal (client)) {

		switch (event->type) {
		case PortConnected:
		case PortDisconnected:
			jack_client_handle_port_connection (client->control->private_client, event);
			break;

		case BufferSizeChange:
			jack_client_invalidate_port_buffers (client->control->private_client);

			if (client->control->bufsize) {
				client->control->bufsize (event->x.n, client->control->bufsize_arg);
			}
			break;

		case SampleRateChange:
			if (client->control->srate) {
				client->control->srate (event->x.n, client->control->bufsize_arg);
			}
			break;

		case GraphReordered:
			if (client->control->graph_order) {
				client->control->graph_order (client->control->graph_order_arg);
			}
			break;

		case XRun:
			if (client->control->xrun) {
				client->control->xrun (client->control->xrun_arg);
			}
			break;

		case NewPortType:
			jack_client_handle_new_port_type (client->control->private_client, 
							  event->x.shm_name, event->z.size, event->y.addr);
			break;

		default:
			/* internal clients don't need to know */
			break;
		}

	} else {

		DEBUG ("engine writing on event fd");

		if (write (client->event_fd, event, sizeof (*event)) != sizeof (*event)) {
			jack_error ("cannot send event to client [%s] (%s)", client->control->name, strerror (errno));
			client->error++;
		}

		DEBUG ("engine reading from event fd");

		if (!client->error && (read (client->event_fd, &status, sizeof (status)) != sizeof (status))) {
			jack_error ("cannot read event response from client [%s] (%s)", client->control->name, strerror (errno));
			client->error++;
		}

		if (status != 0) {
			jack_error ("bad status for client event handling (type = %d)", event->type);
			client->error++;
		}
	}

	DEBUG ("event delivered");

	return 0;
}

int
jack_rechain_graph (jack_engine_t *engine)
{
	JSList *node, *next;
	unsigned long n;
	int err = 0;
	jack_client_internal_t *client, *subgraph_client, *next_client;
	jack_event_t event;

	jack_clear_fifos (engine);

	subgraph_client = 0;

	if (engine->verbose) {
		fprintf(stderr, "++ jack_rechain_graph():\n");
	}

	event.type = GraphReordered;

	for (n = 0, node = engine->clients, next = NULL; node; node = next) {

		next = jack_slist_next (node);

		if (((jack_client_internal_t *) node->data)->control->active) {

			client = (jack_client_internal_t *) node->data;

			/* find the next active client. its ok for this to be NULL */
			
			while (next) {
				if (((jack_client_internal_t *) next->data)->control->active) {
					break;
				}
				next = jack_slist_next (next);
			};

			if (next == NULL) {
				next_client = NULL;
			} else {
				next_client = (jack_client_internal_t *) next->data;
			}

			client->execution_order = n;
			client->next_client = next_client;
			
			if (jack_client_is_internal (client)) {
				
				/* break the chain for the current subgraph. the server
				   will wait for chain on the nth FIFO, and will
				   then execute this internal client.
				*/
				
				if (subgraph_client) {
					subgraph_client->subgraph_wait_fd = jack_get_fifo_fd (engine, n);
					if (engine->verbose) {
						fprintf(stderr, "client %s: wait_fd=%d, execution_order=%lu.\n", 
							subgraph_client->control->name, subgraph_client->subgraph_wait_fd, n);
					}
					n++;
				}

				if (engine->verbose) {
					fprintf(stderr, "client %s: internal client, execution_order=%lu.\n", 
						client->control->name, n);
				}

				/* this does the right thing for internal clients too */

				jack_deliver_event (engine, client, &event);

				subgraph_client = 0;
				
			} else {
				
				if (subgraph_client == NULL) {
					
				        /* start a new subgraph. the engine will start the chain
					   by writing to the nth FIFO.
					*/
					
					subgraph_client = client;
					subgraph_client->subgraph_start_fd = jack_get_fifo_fd (engine, n);
					if (engine->verbose) {
						fprintf(stderr, "client %s: start_fd=%d, execution_order=%lu.\n",
							subgraph_client->control->name, subgraph_client->subgraph_start_fd, n);
					}
				} 
				else {
					if (engine->verbose) {
						fprintf(stderr, "client %s: in subgraph after %s, execution_order=%lu.\n",
							client->control->name, subgraph_client->control->name, n);
					}
					subgraph_client->subgraph_wait_fd = -1;
				}

				/* make sure fifo for 'n + 1' exists 
				 * before issuing client reorder 
				 */
				
				(void) jack_get_fifo_fd(engine, client->execution_order + 1);

				event.x.n = client->execution_order;
				
				jack_deliver_event (engine, client, &event);

				n++;
			}
		}
	}

	if (subgraph_client) {
		subgraph_client->subgraph_wait_fd = jack_get_fifo_fd (engine, n);
		if (engine->verbose) {
			fprintf(stderr, "client %s: wait_fd=%d, execution_order=%lu (last client).\n", 
				subgraph_client->control->name, subgraph_client->subgraph_wait_fd, n);
		}
	}

	if (engine->verbose) {
		fprintf(stderr, "-- jack_rechain_graph()\n");
	}

	return err;
}

static void
jack_trace_terminal (jack_client_internal_t *c1, jack_client_internal_t *rbase)
{
	jack_client_internal_t *c2;

	/* make a copy of the existing list of routes that feed c1. this provides
	   us with an atomic snapshot of c1's "fed-by" state, which will be
	   modified as we progress ...
	*/

	JSList *existing;
	JSList *node;

	if (c1->fed_by == NULL) {
		return;
	}

	existing = jack_slist_copy (c1->fed_by);

	/* for each route that feeds c1, recurse, marking it as feeding
	   rbase as well.
	*/

	for (node = existing; node; node = jack_slist_next  (node)) {

		c2 = (jack_client_internal_t *) node->data;

		/* c2 is a route that feeds c1 which somehow feeds base. mark
		   base as being fed by c2, but don't do it more than
		   once.
		*/

		if (c2 != rbase && c2 != c1) {

			if (jack_slist_find (rbase->fed_by, c2) == NULL) {
				rbase->fed_by = jack_slist_prepend (rbase->fed_by, c2);
			}

			/* FIXME: if c2->fed_by is not up-to-date, we may end up
			          recursing infinitely (kaiv)
			*/

			if (jack_slist_find (c2->fed_by, c1) == NULL) {
				/* now recurse, so that we can mark base as being fed by
				   all routes that feed c2
				*/
				jack_trace_terminal (c2, rbase);
			}
		}
	}

	jack_slist_free (existing);
}

static int 
jack_client_sort (jack_client_internal_t *a, jack_client_internal_t *b)

{
	if (jack_slist_find (a->fed_by, b)) {
		
		if (jack_slist_find (b->fed_by, a)) {

			/* feedback loop: if `a' is the driver
			   client, let that execute first.
			*/

			if (a->control->type == ClientDriver) {
				/* b comes after a */
				return -1;
			}
		}

		/* a comes after b */
		return 1;

	} else if (jack_slist_find (b->fed_by, a)) {
		
		if (jack_slist_find (a->fed_by, b)) {

			/* feedback loop: if `b' is the driver
			   client, let that execute first.
			*/

			if (b->control->type == ClientDriver) {
				/* b comes before a */
				return 1;
			}
		}

		/* b comes after a */
		return -1;
	} else {
		/* we don't care */
		return 0;
	}
}

static int
jack_client_feeds (jack_client_internal_t *might, jack_client_internal_t *target)
{
	JSList *pnode, *cnode;

	/* Check every port of `might' for an outbound connection to `target'
	*/

	for (pnode = might->ports; pnode; pnode = jack_slist_next (pnode)) {

		jack_port_internal_t *port;
		
		port = (jack_port_internal_t *) pnode->data;

		for (cnode = port->connections; cnode; cnode = jack_slist_next (cnode)) {

			jack_connection_internal_t *c;

			c = (jack_connection_internal_t *) cnode->data;

			if (c->source->shared->client_id == might->control->id &&
			    c->destination->shared->client_id == target->control->id) {
				return 1;
			}
		}
	}
	
	return 0;
}

static jack_nframes_t
jack_get_port_total_latency (jack_engine_t *engine, jack_port_internal_t *port, int hop_count, int toward_port)
{
	JSList *node;
	jack_nframes_t latency;
	jack_nframes_t max_latency = 0;

	/* call tree must hold engine->client_lock. */
	
	latency = port->shared->latency;

	/* we don't prevent cyclic graphs, so we have to do something to bottom out
	   in the event that they are created.
	*/

	if (hop_count > 8) {
		return latency;
	}

	for (node = port->connections; node; node = jack_slist_next (node)) {

		jack_nframes_t this_latency;
		jack_connection_internal_t *connection;

		connection = (jack_connection_internal_t *) node->data;

		
		if ((toward_port && (connection->source->shared == port->shared)) ||
		    (!toward_port && (connection->destination->shared == port->shared))) {
			continue;
		}

		/* if we're a destination in the connection, recurse on the source to
		   get its total latency
		*/
		
		if (connection->destination == port) {

			if (connection->source->shared->flags & JackPortIsTerminal) {
				this_latency = connection->source->shared->latency;
			} else {
				this_latency = jack_get_port_total_latency (engine, connection->source, hop_count + 1, 
									    toward_port);
			}

		} else {

			/* "port" is the source, so get the latency of the destination */

			if (connection->destination->shared->flags & JackPortIsTerminal) {
				this_latency = connection->destination->shared->latency;
			} else {
				this_latency = jack_get_port_total_latency (engine, connection->destination, hop_count + 1, 
									    toward_port);
			}
		}

		if (this_latency > max_latency) {
			max_latency = this_latency;
		}
	}

	return latency + max_latency;
}

static void
jack_compute_all_port_total_latencies (jack_engine_t *engine)
{
	jack_port_shared_t *shared = engine->control->ports;
	unsigned int i;
	int toward_port;

	for (i = 0; i < engine->control->port_max; i++) {
		if (shared[i].in_use) {
			if (shared[i].flags & JackPortIsOutput) {
				toward_port = FALSE;
			} else {
				toward_port = TRUE;
			}
			shared[i].total_latency = jack_get_port_total_latency (engine, &engine->internal_ports[i], 0, toward_port);
		}
	}
}

/**
 * Sorts the network of clients using the following 
 * algorithm:
 *
 * 1) figure out who is connected to whom:
 *    
 *    foreach client1
 *       foreach input port
 *           foreach client2
 *              foreach output port
 *                 if client1->input port connected to client2->output port
 *                     mark client1 fed by client 2
 *
 * 2) trace the connections as terminal arcs in the graph so that
 *    if client A feeds client B who feeds client C, mark client C
 *    as fed by client A as well as client B, and so forth.
 *
 * 3) now sort according to whether or not client1->fed_by (client2) is true.
 *    if the condition is true, client2 must execute before client1
 *
 */

static void
jack_sort_graph (jack_engine_t *engine)
{
	JSList *node, *onode;
	jack_client_internal_t *client;
	jack_client_internal_t *oclient;

	/* called, obviously, must hold engine->client_lock */

	for (node = engine->clients; node; node = jack_slist_next (node)) {

		client = (jack_client_internal_t *) node->data;

		jack_slist_free (client->fed_by);
		client->fed_by = 0;

		for (onode = engine->clients; onode; onode = jack_slist_next (onode)) {
			
			oclient = (jack_client_internal_t *) onode->data;

			if (jack_client_feeds (oclient, client)) {
				client->fed_by = jack_slist_prepend (client->fed_by, oclient);
			}
		}
	}

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		jack_trace_terminal ((jack_client_internal_t *) node->data,
				     (jack_client_internal_t *) node->data);
	}

	engine->clients = jack_slist_sort (engine->clients, (JCompareFunc) jack_client_sort);

	jack_compute_all_port_total_latencies (engine);

	jack_rechain_graph (engine);
}

/**
 * Dumps current engine configuration to stderr.
 */
void jack_dump_configuration(jack_engine_t *engine, int take_lock)
{
        JSList *clientnode, *portnode, *connectionnode;
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	jack_port_internal_t *port;
	jack_connection_internal_t* connection;
	int n, m, o;
	
	fprintf(stderr, "engine.c: <-- dump begins -->\n");

	if (take_lock) {
		jack_lock_graph (engine);
	}

	for (n = 0, clientnode = engine->clients; clientnode; clientnode = jack_slist_next (clientnode)) {
	        client = (jack_client_internal_t *) clientnode->data;
		ctl = client->control;

		fprintf (stderr, "client #%d: %s (type: %d, process? %s, fed by %d clients) start=%d wait=%d\n",
			 ++n,
			 ctl->name,
			 ctl->type,
			 ctl->process ? "yes" : "no",
			 jack_slist_length(client->fed_by),
			 client->subgraph_start_fd,
			 client->subgraph_wait_fd);
		
		for(m = 0, portnode = client->ports; portnode; portnode = jack_slist_next (portnode)) {
		        port = (jack_port_internal_t *) portnode->data;

			fprintf(stderr, "\t port #%d: %s\n", ++m, port->shared->name);

			for(o = 0, connectionnode = port->connections; 
			    connectionnode; 
			    connectionnode = jack_slist_next (connectionnode)) {
			        connection = (jack_connection_internal_t *) connectionnode->data;
	
				fprintf(stderr, "\t\t connection #%d: %s %s\n",
					++o,
					(port->shared->flags & JackPortIsInput) ? "<-" : "->",
					(port->shared->flags & JackPortIsInput) ?
					  connection->source->shared->name :
					  connection->destination->shared->name);
			}
		}
	}

	if (take_lock) {
		jack_unlock_graph (engine);
	}

	
	fprintf(stderr, "engine.c: <-- dump ends -->\n");
}

static int 
jack_port_do_connect (jack_engine_t *engine,
		       const char *source_port,
		       const char *destination_port)
{
	jack_connection_internal_t *connection;
	jack_port_internal_t *srcport, *dstport;
	jack_port_id_t src_id, dst_id;
	jack_client_internal_t *client;

	if ((srcport = jack_get_port_by_name (engine, source_port)) == NULL) {
		jack_error ("unknown source port in attempted connection [%s]", source_port);
		return -1;
	}

	if ((dstport = jack_get_port_by_name (engine, destination_port)) == NULL) {
		jack_error ("unknown destination port in attempted connection [%s]", destination_port);
		return -1;
	}

	if ((dstport->shared->flags & JackPortIsInput) == 0) {
		jack_error ("destination port in attempted connection of %s and %s is not an input port", 
			    source_port, destination_port);
		return -1;
	}

	if ((srcport->shared->flags & JackPortIsOutput) == 0) {
		jack_error ("source port in attempted connection of %s and %s is not an output port",
			    source_port, destination_port);
		return -1;
	}

	if (srcport->shared->locked) {
		jack_error ("source port %s is locked against connection changes", source_port);
		return -1;
	}

	if (dstport->shared->locked) {
		jack_error ("destination port %s is locked against connection changes", destination_port);
		return -1;
	}

	if (srcport->shared->type_info.type_id != dstport->shared->type_info.type_id) {
		jack_error ("ports used in attemped connection are not of the same data type");
		return -1;
	}

	if ((client = jack_client_internal_by_id (engine, srcport->shared->client_id)) == 0) {
		jack_error ("unknown client set as owner of port - cannot connect");
		return -1;
	}
	
	if (!client->control->active) {
		jack_error ("cannot connect ports owned by inactive clients; \"%s\" is not active", client->control->name);
		return -1;
	}

	if ((client = jack_client_internal_by_id (engine, dstport->shared->client_id)) == 0) {
		jack_error ("unknown client set as owner of port - cannot connect");
		return -1;
	}
	
	if (!client->control->active) {
		jack_error ("cannot connect ports owned by inactive clients; \"%s\" is not active", client->control->name);
		return -1;
	}

	connection = (jack_connection_internal_t *) malloc (sizeof (jack_connection_internal_t));

	connection->source = srcport;
	connection->destination = dstport;

	src_id = srcport->shared->id;
	dst_id = dstport->shared->id;

	jack_lock_graph (engine);

	if (dstport->connections && dstport->shared->type_info.mixdown == NULL) {
		jack_error ("cannot make multiple connections to a port of type [%s]", 
			    dstport->shared->type_info.type_name);
		free (connection);
		jack_unlock_graph (engine);
		return -1;
	} else {
		if (engine->verbose) {
			fprintf (stderr, "connect %s and %s\n",
				 srcport->shared->name,
				 dstport->shared->name);
		}

		dstport->connections = jack_slist_prepend (dstport->connections, connection);
		srcport->connections = jack_slist_prepend (srcport->connections, connection);
		
		jack_sort_graph (engine);

		DEBUG ("actually sorted the graph...");

		jack_send_connection_notification (engine, srcport->shared->client_id, src_id, dst_id, TRUE);
		jack_send_connection_notification (engine, dstport->shared->client_id, dst_id, src_id, TRUE);

	}

	jack_unlock_graph (engine);
	return 0;
}

int
jack_port_disconnect_internal (jack_engine_t *engine, 
			       jack_port_internal_t *srcport, 
			       jack_port_internal_t *dstport, 
			       int sort_graph)

{
	JSList *node;
	jack_connection_internal_t *connect;
	int ret = -1;
	jack_port_id_t src_id, dst_id;

	/* call tree **** MUST HOLD **** engine->client_lock. */
	
	for (node = srcport->connections; node; node = jack_slist_next (node)) {

		connect = (jack_connection_internal_t *) node->data;

		if (connect->source == srcport && connect->destination == dstport) {

			if (engine->verbose) {
				fprintf (stderr, "DIS-connect %s and %s\n",
					 srcport->shared->name,
					 dstport->shared->name);
			}
			
			srcport->connections = jack_slist_remove (srcport->connections, connect);
			dstport->connections = jack_slist_remove (dstport->connections, connect);

			src_id = srcport->shared->id;
			dst_id = dstport->shared->id;

			/* this is a bit harsh, but it basically says that if we actually
			   do a disconnect, and its the last one, then make sure that
			   any input monitoring is turned off on the srcport. this isn't
			   ideal for all situations, but it works better for most of them.
			*/

			if (srcport->connections == NULL) {
				srcport->shared->monitor_requests = 0;
			}

			jack_send_connection_notification (engine, srcport->shared->client_id, src_id, dst_id, FALSE);
			jack_send_connection_notification (engine, dstport->shared->client_id, dst_id, src_id, FALSE);

			free (connect);
			ret = 0;
			break;
		}
	}

	if (sort_graph) {
		jack_sort_graph (engine);
	}

	return ret;
}

static int
jack_port_do_disconnect_all (jack_engine_t *engine,
			     jack_port_id_t port_id)
{
	if (port_id >= engine->control->port_max) {
		jack_error ("illegal port ID in attempted disconnection [%u]", port_id);
		return -1;
	}

	if (engine->verbose) {
		fprintf (stderr, "clear connections for %s\n", engine->internal_ports[port_id].shared->name);
	}

	jack_lock_graph (engine);
	jack_port_clear_connections (engine, &engine->internal_ports[port_id]);
	jack_sort_graph (engine);
	jack_unlock_graph (engine);

	return 0;
}

static int 
jack_port_do_disconnect (jack_engine_t *engine,
			 const char *source_port,
			 const char *destination_port)
{
	jack_port_internal_t *srcport, *dstport;
	int ret = -1;

	if ((srcport = jack_get_port_by_name (engine, source_port)) == NULL) {
		jack_error ("unknown source port in attempted disconnection [%s]", source_port);
		return -1;
	}

	if ((dstport = jack_get_port_by_name (engine, destination_port)) == NULL) {
		jack_error ("unknown destination port in attempted connection [%s]", destination_port);
		return -1;
	}

	jack_lock_graph (engine);

	ret = jack_port_disconnect_internal (engine, srcport, dstport, TRUE);

	jack_unlock_graph (engine);

	return ret;
}

static int 
jack_get_fifo_fd (jack_engine_t *engine, unsigned int which_fifo)

{
	/* caller must hold client_lock */

	char path[PATH_MAX+1];
	struct stat statbuf;

	snprintf (path, sizeof (path), "%s-%d", engine->fifo_prefix, which_fifo);

	DEBUG ("%s", path);

	if (stat (path, &statbuf)) {
		if (errno == ENOENT) {
       
                #if defined(linux) 
                        if (mknod (path, 0666|S_IFIFO, 0) < 0) {
                #elif defined(__APPLE__) && defined(__POWERPC__) 
                        if (mkfifo(path,0666) < 0){
                #endif
				jack_error ("cannot create inter-client FIFO [%s] (%s)\n", path, strerror (errno));
				return -1;
			}
		} else {
			jack_error ("cannot check on FIFO %d\n", which_fifo);
			return -1;
		}
	} else {
		if (!S_ISFIFO(statbuf.st_mode)) {
			jack_error ("FIFO %d (%s) already exists, but is not a FIFO!\n", which_fifo, path);
			return -1;
		}
	}

	if (which_fifo >= engine->fifo_size) {
		unsigned int i;

		engine->fifo = (int *) realloc (engine->fifo, sizeof (int) * engine->fifo_size + 16);
		for (i = engine->fifo_size; i < engine->fifo_size + 16; i++) {
			engine->fifo[i] = -1;
		}
		engine->fifo_size += 16;
	}

	if (engine->fifo[which_fifo] < 0) {
		if ((engine->fifo[which_fifo] = open (path, O_RDWR|O_CREAT|O_NONBLOCK, 0666)) < 0) {
			jack_error ("cannot open fifo [%s] (%s)", path, strerror (errno));
			return -1;
		}
		DEBUG ("opened engine->fifo[%d] == %d (%s)", which_fifo, engine->fifo[which_fifo], path);
	}

	return engine->fifo[which_fifo];
}

static void
jack_clear_fifos (jack_engine_t *engine)
{
	/* caller must hold client_lock */

	unsigned int i;
	char buf[16];

	/* this just drains the existing FIFO's of any data left in them
	   by aborted clients, etc. there is only ever going to be
	   0, 1 or 2 bytes in them, but we'll allow for up to 16.
	*/

	for (i = 0; i < engine->fifo_size; i++) {
		if (engine->fifo[i] >= 0) {
			int nread = read (engine->fifo[i], buf, sizeof (buf));

			if (nread < 0 && errno != EAGAIN) {
				jack_error ("clear fifo[%d] error: %s", i, strerror (errno));
			} 
		}
	}
}

static int
jack_use_driver (jack_engine_t *engine, jack_driver_t *driver)
{
	if (engine->driver) {
		engine->driver->detach (engine->driver, engine);
		engine->driver = 0;
	}

	if (driver) {
		if (driver->attach (driver, engine)) {
			return -1;
		}

		engine->rolling_interval = (int) floor ((JACK_ENGINE_ROLLING_INTERVAL * 1000.0f) / driver->period_usecs);
	}

	engine->driver = driver;
	return 0;
}

/* PORT RELATED FUNCTIONS */


static jack_port_id_t
jack_get_free_port (jack_engine_t *engine)

{
	jack_port_id_t i;

	pthread_mutex_lock (&engine->port_lock);

	for (i = 0; i < engine->port_max; i++) {
		if (engine->control->ports[i].in_use == 0) {
			engine->control->ports[i].in_use = 1;
			break;
		}
	}
	
	pthread_mutex_unlock (&engine->port_lock);
	
	if (i == engine->port_max) {
		return (jack_port_id_t) -1;
	}

	return i;
}

static void
jack_port_release (jack_engine_t *engine, jack_port_internal_t *port)
{
	pthread_mutex_lock (&engine->port_lock);
	port->shared->in_use = 0;

	if (port->buffer_info) {
		jack_port_type_info_t *info = jack_global_port_type_info (engine, port);
		pthread_mutex_lock (&info->buffer_lock);
		info->buffer_freelist = jack_slist_prepend (info->buffer_freelist, port->buffer_info);
		port->buffer_info = NULL;
		pthread_mutex_unlock (&info->buffer_lock);
	}
	pthread_mutex_unlock (&engine->port_lock);
}

jack_port_internal_t *
jack_get_port_internal_by_name (jack_engine_t *engine, const char *name)
{
	jack_port_id_t id;

	pthread_mutex_lock (&engine->port_lock);

	for (id = 0; id < engine->port_max; id++) {
		if (strcmp (engine->control->ports[id].name, name) == 0) {
			break;
		}
	}

	pthread_mutex_unlock (&engine->port_lock);
	
	if (id != engine->port_max) {
		return &engine->internal_ports[id];
	} else {
		return NULL;
	}
}

int
jack_port_do_register (jack_engine_t *engine, jack_request_t *req)
	
{
	jack_port_id_t port_id;
	jack_port_shared_t *shared;
	jack_port_internal_t *port;
	jack_client_internal_t *client;
	unsigned long i;

	for (i = 0; i < engine->control->n_port_types; ++i) {
		if (strcmp (req->x.port_info.type, engine->control->port_types[i].type_name) == 0) {
			break;
		}
	}

	if (i == engine->control->n_port_types) {
		jack_error ("cannot register a port of type \"%s\"", req->x.port_info.type);
		return -1;
	}

	jack_lock_graph (engine);
	if ((client = jack_client_internal_by_id (engine, req->x.port_info.client_id)) == NULL) {
		jack_error ("unknown client id in port registration request");
		return -1;
	}
	jack_unlock_graph (engine);

	if ((port_id = jack_get_free_port (engine)) == (jack_port_id_t) -1) {
		jack_error ("no ports available!");
		return -1;
	}

	shared = &engine->control->ports[port_id];

	strcpy (shared->name, req->x.port_info.name);
	memcpy (&shared->type_info, &engine->control->port_types[i], sizeof (jack_port_type_info_t));
	shared->client_id = req->x.port_info.client_id;
	shared->flags = req->x.port_info.flags;
	shared->latency = 0;
	shared->monitor_requests = 0;
	shared->locked = 0;

	fprintf (stderr, "port %s has mixdown = %p\n", shared->name, shared->type_info.mixdown);

	port = &engine->internal_ports[port_id];

	port->shared = shared;
	port->connections = 0;

	if (jack_port_assign_buffer (engine, port)) {
		jack_error ("cannot assign buffer for port");
		return -1;
	}

	jack_lock_graph (engine);
	client->ports = jack_slist_prepend (client->ports, port);
	jack_port_registration_notify (engine, port_id, TRUE);
	jack_unlock_graph (engine);

	if (engine->verbose) {
		fprintf (stderr, "registered port %s, offset = %u\n", shared->name, (unsigned int)shared->offset);
	}

	req->x.port_info.port_id = port_id;

	return 0;
}

int
jack_port_do_unregister (jack_engine_t *engine, jack_request_t *req)
{
	jack_client_internal_t *client;
	jack_port_shared_t *shared;
	jack_port_internal_t *port;

	if (req->x.port_info.port_id < 0 || req->x.port_info.port_id > engine->port_max) {
		jack_error ("invalid port ID %d in unregister request", req->x.port_info.port_id);
		return -1;
	}

	shared = &engine->control->ports[req->x.port_info.port_id];

	if (shared->client_id != req->x.port_info.client_id) {
		jack_error ("Client %d is not allowed to remove port %s", req->x.port_info.client_id, shared->name);
		return -1;
	}

	jack_lock_graph (engine);
	if ((client = jack_client_internal_by_id (engine, shared->client_id)) == NULL) {
		jack_error ("unknown client id in port registration request");
		jack_unlock_graph (engine);
		return -1;
	}

	port = &engine->internal_ports[req->x.port_info.port_id];

	jack_port_clear_connections (engine, port);
	jack_port_release (engine, &engine->internal_ports[req->x.port_info.port_id]);
	
	client->ports = jack_slist_remove (client->ports, port);
	jack_port_registration_notify (engine, req->x.port_info.port_id, FALSE);
	jack_unlock_graph (engine);

	return 0;
}

int
jack_do_get_port_connections (jack_engine_t *engine, jack_request_t *req, int reply_fd)
{
	jack_port_internal_t *port;
	JSList *node;
	unsigned int i;
	int ret = -1;
	int internal = FALSE;

	jack_lock_graph (engine);

	port = &engine->internal_ports[req->x.port_info.port_id];

	DEBUG ("Getting connections for port '%s'.", port->shared->name);

	req->x.port_connections.nports = jack_slist_length (port->connections);
	req->status = 0;

	/* figure out if this is an internal or external client */

	for (node = engine->clients; node; node = jack_slist_next (node)) {
		
		if (((jack_client_internal_t *) node->data)->request_fd == reply_fd) {
			internal = jack_client_is_internal((jack_client_internal_t *) node->data);
			break;
		}
	}

	if (!internal) {
		if (write (reply_fd, req, sizeof (*req)) < (ssize_t) sizeof (req)) {
			jack_error ("cannot write GetPortConnections result to client via fd = %d (%s)", 
				    reply_fd, strerror (errno));
			goto out;
		}
	} else {
		req->x.port_connections.ports = (const char **) malloc (sizeof (char *) * req->x.port_connections.nports);
	}

	if (req->type == GetPortConnections) {
		
		for (i = 0, node = port->connections; node; node = jack_slist_next (node), ++i) {

			jack_port_id_t port_id;
			
			if (((jack_connection_internal_t *) node->data)->source == port) {
				port_id = ((jack_connection_internal_t *) node->data)->destination->shared->id;
			} else {
				port_id = ((jack_connection_internal_t *) node->data)->source->shared->id;
			}
			
			if (internal) {

				/* internal client asking for names. store in malloc'ed space, client frees
				 */

				req->x.port_connections.ports[i] = engine->control->ports[port_id].name;

			} else {

				/* external client asking for names. we write the port id's to the reply fd.
				 */

				if (write (reply_fd, &port_id, sizeof (port_id)) < (ssize_t) sizeof (port_id)) {
					jack_error ("cannot write port id to client");
					goto out;
				}
			}
		}
	}

	ret = 0;

  out:
	req->status = ret;
	jack_unlock_graph (engine);
	return ret;
}

void
jack_port_registration_notify (jack_engine_t *engine, jack_port_id_t port_id, int yn)

{
	jack_event_t event;
	jack_client_internal_t *client;
	JSList *node;

	event.type = (yn ? PortRegistered : PortUnregistered);
	event.x.port_id = port_id;
	
	for (node = engine->clients; node; node = jack_slist_next (node)) {
		
		client = (jack_client_internal_t *) node->data;

		if (!client->control->active) {
			continue;
		}

		if (client->control->port_register) {
			if (jack_deliver_event (engine, client, &event)) {
				jack_error ("cannot send port registration notification to %s (%s)",
					     client->control->name, strerror (errno));
			}
		}
	}
}

int
jack_port_assign_buffer (jack_engine_t *engine, jack_port_internal_t *port)
{
	jack_port_type_info_t *port_type = jack_global_port_type_info (engine, port);
	jack_port_buffer_info_t *bi;

	if (port->shared->flags & JackPortIsInput) {
		port->shared->offset = 0;
		return 0;
	}
	
	pthread_mutex_lock (&port_type->buffer_lock);

	if (port_type->buffer_freelist == NULL) {
		jack_error ("all %s port buffers in use!", port_type->type_name);
		pthread_mutex_unlock (&port_type->buffer_lock);
		return -1;
	}

	bi = (jack_port_buffer_info_t *) port_type->buffer_freelist->data;
	port_type->buffer_freelist = jack_slist_remove (port_type->buffer_freelist, bi);

	port->shared->offset = bi->offset;
	port->buffer_info = bi;

	pthread_mutex_unlock (&port_type->buffer_lock);
	return 0;
}

static jack_port_internal_t *
jack_get_port_by_name (jack_engine_t *engine, const char *name)

{
	jack_port_id_t id;

	/* Note the potential race on "in_use". Other design
	   elements prevent this from being a problem.
	*/

	for (id = 0; id < engine->port_max; id++) {
		if (engine->control->ports[id].in_use && strcmp (engine->control->ports[id].name, name) == 0) {
			return &engine->internal_ports[id];
		}
	}

	return NULL;
}

static int
jack_send_connection_notification (jack_engine_t *engine, jack_client_id_t client_id, 
				   jack_port_id_t self_id, jack_port_id_t other_id, int connected)

{
	jack_client_internal_t *client;
	jack_event_t event;

	if ((client = jack_client_internal_by_id (engine, client_id)) == NULL) {
		jack_error ("no such client %d during connection notification", client_id);
		return -1;
	}

	if (client->control->active) {
		event.type = (connected ? PortConnected : PortDisconnected);
		event.x.self_id = self_id;
		event.y.other_id = other_id;
		
		if (jack_deliver_event (engine, client, &event)) {
			jack_error ("cannot send port connection notification to client %s (%s)", 
				    client->control->name, strerror (errno));
			return -1;
		}
	}

	return 0;
}

void
jack_set_asio_mode (jack_engine_t *engine, int yn)
{
	engine->asio_mode = yn;
}


