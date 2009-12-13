// +------------------------------------------------------------------+
// |             ____ _               _        __  __ _  __           |
// |            / ___| |__   ___  ___| | __   |  \/  | |/ /           |
// |           | |   | '_ \ / _ \/ __| |/ /   | |\/| | ' /            |
// |           | |___| | | |  __/ (__|   <    | |  | | . \            |
// |            \____|_| |_|\___|\___|_|\_\___|_|  |_|_|\_\           |
// |                                                                  |
// | Copyright Mathias Kettner 2009             mk@mathias-kettner.de |
// +------------------------------------------------------------------+
// 
// This file is part of Check_MK.
// The official homepage is at http://mathias-kettner.de/check_mk.
// 
// check_mk is free software;  you can redistribute it and/or modify it
// under the  terms of the  GNU General Public License  as published by
// the Free Software Foundation in version 2.  check_mk is  distributed
// in the hope that it will be useful, but WITHOUT ANY WARRANTY;  with-
// out even the implied warranty of  MERCHANTABILITY  or  FITNESS FOR A
// PARTICULAR PURPOSE. See the  GNU General Public License for more de-
// ails.  You should have  received  a copy of the  GNU  General Public
// License along with GNU Make; see the file  COPYING.  If  not,  write
// to the Free Software Foundation, Inc., 51 Franklin St,  Fifth Floor,
// Boston, MA 02110-1301 USA.

#include <sys/select.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include "livestatus.h"
#include "nagios.h"
#include "store.h"
#include "logger.h"
#include "config.h"
#include "global_counters.h"

#ifndef AF_LOCAL
#define   AF_LOCAL AF_UNIX
#endif
#ifndef PF_LOCAL
#define   PF_LOCAL PF_UNIX
#endif

NEB_API_VERSION(CURRENT_NEB_API_VERSION)

#define ACCEPT_TIMEOUT_USEC 250000
#define NUM_CLIENTTHREADS   10 /* maybe make configurable */

#define false 0
#define true 1


void *g_nagios_handle;
int g_unix_socket = -1;
int g_max_fd_ever = 0;
char g_socket_path[4096];
int g_should_terminate = false;
pthread_t g_mainthread_id;
pthread_t g_clientthread_id[NUM_CLIENTTHREADS];

int g_thread_running = 0;
int g_thread_pid = 0;

void livestatus_cleanup_after_fork()
{
   store_deinit();
   struct stat st;
   int i;
   // We need to close our server and client sockets. Otherwise
   // our connections are inherited to host and service checks.
   // If we close our client connection in such a situation,
   // the connection will still be open since and the client will
   // hang while trying to read further data. And the CLOEXEC is
   // not atomic :-(
   for (i=3; i < g_max_fd_ever; i++) {
      if (0 == fstat(i, &st) && S_ISSOCK(st.st_mode))
      {
	 close(i);
      }
   }
}

void *main_thread(void *data)
{
   g_thread_pid = getpid();
   logger(LG_INFO, "Entering main loop, listening on UNIX socket. PID is %d", g_thread_pid);
   while (!g_should_terminate) 
   {
      do_statistics();
      if (g_thread_pid != getpid()) {
	 logger(LG_INFO, "I'm not the main process but %d!", getpid());
	 // return;
      }
      struct timeval tv;
      tv.tv_sec  = ACCEPT_TIMEOUT_USEC / 1000000;
      tv.tv_usec = ACCEPT_TIMEOUT_USEC % 1000000;

      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(g_unix_socket, &fds);
      int retval = select(g_unix_socket + 1, &fds, NULL, NULL, &tv);
      if (retval > 0 && FD_ISSET(g_unix_socket, &fds)) {
	 int cc = accept(g_unix_socket, NULL, NULL);
	 if (cc > g_max_fd_ever)
	    g_max_fd_ever = cc;
	 if (0 < fcntl(cc, F_SETFD, FD_CLOEXEC))
	   logger(LG_INFO, "Cannot set FD_CLOEXEC on client socket: %s", strerror(errno));
	 queue_add_connection(cc); // closes fd
	 g_counters[COUNTER_CONNECTIONS]++;
      }
   }
   logger(LG_INFO, "Socket thread has terminated");
}


void *client_thread(void *data)
{
   void *input_buffer = create_inputbuffer(&g_should_terminate);
   void *output_buffer = create_outputbuffer();
   logger(LG_INFO, "Starting client thread %p", output_buffer);

   while (!g_should_terminate) {
      int cc = queue_pop_connection();
      if (cc >= 0) {
	 set_inputbuffer_fd(input_buffer, cc);
	 int keepalive = 1;
	 while (keepalive) {
	    keepalive = store_answer_request(input_buffer, output_buffer);
	    flush_output_buffer(output_buffer, cc, &g_should_terminate);
	    g_counters[COUNTER_REQUESTS]++;
	 }
	 close(cc);
      }
   }
   delete_outputbuffer(output_buffer);
   delete_inputbuffer(input_buffer);
   logger(LG_INFO, "Client thread %p has terminated", output_buffer);
}

void start_threads()
{
   if (!g_thread_running) {
      /* start thread that listens on socket */
      pthread_atfork(NULL, NULL, livestatus_cleanup_after_fork);
      pthread_create(&g_mainthread_id, 0, main_thread, (void *)0);

      unsigned t;
      for (t=0; t < NUM_CLIENTTHREADS; t++)
	 pthread_create(&g_clientthread_id[t], 0, client_thread, (void *)0);

      g_thread_running = 1;
   }
}

void terminate_threads()
{
   if (g_thread_running) {
      g_should_terminate = true;
      logger(LG_INFO, "Waiting for main to terminate...");
      pthread_join(g_mainthread_id, NULL);
      logger(LG_INFO, "Waiting for client threads to terminate...");
      queue_wakeup_all();
      unsigned t;
      for (t=0; t < NUM_CLIENTTHREADS; t++) {
	 if (0 != pthread_join(g_clientthread_id[t], NULL))
	    logger(LG_INFO, "Warning: could not join thread %p", g_clientthread_id[t]);
      }
      logger(LG_INFO, "Main thread + %u client threads have finished", NUM_CLIENTTHREADS);
      g_thread_running = 0;
   }
}

int open_unix_socket()
{
   struct stat st;
   if (0 == stat(g_socket_path, &st)) {
      if (0 == unlink(g_socket_path)) {
	 logger(LG_DEBUG , "Removed old left over socket file %s", g_socket_path);
      }
      else {
	 logger(LG_ALERT, "Cannot remove in the way file %s: %s",
	       g_socket_path, strerror(errno));
	 return false;
      }
   }

   g_unix_socket = socket(PF_LOCAL, SOCK_STREAM, 0);
   g_max_fd_ever = g_unix_socket;
   if (g_unix_socket < 0)
   {
      logger(LG_CRIT , "Unable to create UNIX socket: %s", strerror(errno));
      return false;
   }
   logger(LG_INFO , "Created UNIX control socket at %s", g_socket_path);

   // Imortant: close on exec -> check plugins must not inherit it!
   if (0 < fcntl(g_unix_socket, F_SETFD, FD_CLOEXEC))
     logger(LG_INFO, "Cannot set FD_CLOEXEC on socket: %s", strerror(errno));

   // Bind it to its address. This creates the file with the name g_socket_path
   struct sockaddr_un sockaddr;
   sockaddr.sun_family = AF_LOCAL;
   strncpy(sockaddr.sun_path, g_socket_path, sizeof(sockaddr.sun_path));
   if (bind(g_unix_socket, (struct sockaddr *) &sockaddr, SUN_LEN(&sockaddr)) < 0)
   {
      logger(LG_ERR , "Unable to bind adress %s to UNIX socket: %s",
	    g_socket_path, strerror(errno));
      close(g_unix_socket);
      return false;
   }

   // Make writable group members (fchmod didn't do nothing for me. Don't know why!)
   if (0 != chmod(g_socket_path, 0660)) {
      logger(LG_ERR , "Cannot chown unix socket at %s to 0660: %s", g_socket_path, strerror(errno));
      close(g_unix_socket);
      return false;
   }

   if (0 != listen(g_unix_socket, 3 /* backlog */)) {
      logger(LG_ERR , "Cannot listen to unix socket at %s: %s", g_socket_path, strerror(errno));
      close(g_unix_socket);
      return false;
   }

   logger(LG_INFO, "Opened UNIX socket %s\n", g_socket_path);
   return true;

}

void close_unix_socket()
{
   unlink(g_socket_path);
   if (g_unix_socket >= 0) {
      close(g_unix_socket);
      g_unix_socket = -1;
   }
}

int broker_host(int event_type, void *data)
{
   nebstruct_host_status_data *hst = (nebstruct_host_status_data *) data;
   host *h = (host *)hst->object_ptr;
   store_register_host(h);
   customvariablesmember *cvm = h->custom_variables;
   while (cvm) {
      cvm = cvm->next;
   }
   start_threads();
   g_counters[COUNTER_NEB_CALLBACKS]++;
}

/* Dies wird aufgerufen, wenn sich an einem Service was aendert. 
   Auch beim Start des Programmes. Eigentlich wuerde uns das reichen,
   weil wir spaeter direkt auf die Services zugreifen. */
int broker_service(int event_type, void *data)
{
   nebstruct_service_status_data *svc = (nebstruct_service_status_data *) data;
   service *s = (service *)svc->object_ptr;
   store_register_service(s);
   g_counters[COUNTER_NEB_CALLBACKS]++;
}

int broker_check(int event_type, void *data)
{
   if (event_type == NEBCALLBACK_SERVICE_CHECK_DATA) {
      nebstruct_service_check_data *c = (nebstruct_service_check_data *)data;
      if (c->type == NEBTYPE_SERVICECHECK_PROCESSED) {
	 g_counters[COUNTER_SERVICE_CHECKS]++;
      }
   }
   else if (event_type == NEBCALLBACK_HOST_CHECK_DATA) {
      nebstruct_host_check_data *c = (nebstruct_host_check_data *)data;
      if (c->type == NEBTYPE_HOSTCHECK_PROCESSED) {
	 g_counters[COUNTER_HOST_CHECKS]++;
      }
   }
}


int broker_contact(int event_type, void *data)
{
   nebstruct_contact_status_data *ctc = (nebstruct_contact_status_data *) data;
   contact *c = (contact *)ctc->object_ptr;
   store_register_contact(c);
   g_counters[COUNTER_NEB_CALLBACKS]++;
}

int broker_comment(int event_type, void *data)
{
   nebstruct_comment_data *co = (nebstruct_comment_data *)data;
   store_register_comment(co);
   g_counters[COUNTER_NEB_CALLBACKS]++;
}

int broker_downtime(int event_type, void *data)
{
   nebstruct_downtime_data *dt = (nebstruct_downtime_data *)data;
   store_register_downtime(dt);
   g_counters[COUNTER_NEB_CALLBACKS]++;
}

void register_callbacks()
{
   neb_register_callback(NEBCALLBACK_HOST_STATUS_DATA,      g_nagios_handle, 0, broker_host);
   neb_register_callback(NEBCALLBACK_SERVICE_STATUS_DATA,   g_nagios_handle, 0, broker_service);
   neb_register_callback(NEBCALLBACK_CONTACT_STATUS_DATA,   g_nagios_handle, 0, broker_contact);
   neb_register_callback(NEBCALLBACK_COMMENT_DATA,          g_nagios_handle, 0, broker_comment);
   neb_register_callback(NEBCALLBACK_DOWNTIME_DATA,         g_nagios_handle, 0, broker_downtime);
   neb_register_callback(NEBCALLBACK_SERVICE_CHECK_DATA,    g_nagios_handle, 0, broker_check);
   neb_register_callback(NEBCALLBACK_HOST_CHECK_DATA,       g_nagios_handle, 0, broker_check);
}

void deregister_callbacks()
{
   neb_deregister_callback(NEBCALLBACK_HOST_STATUS_DATA,      broker_host);
   neb_deregister_callback(NEBCALLBACK_SERVICE_STATUS_DATA,   broker_service);
   neb_deregister_callback(NEBCALLBACK_CONTACT_STATUS_DATA,   broker_contact);
   neb_deregister_callback(NEBCALLBACK_COMMENT_DATA,          broker_downtime);
   neb_deregister_callback(NEBCALLBACK_DOWNTIME_DATA,         broker_downtime);
   neb_deregister_callback(NEBCALLBACK_SERVICE_CHECK_DATA,    broker_downtime);
   neb_deregister_callback(NEBCALLBACK_HOST_CHECK_DATA,       broker_downtime);
}


/* this function gets called when the module is loaded by the event broker */
int nebmodule_init(int flags, char *args, void *handle) 
{
   g_nagios_handle = handle;
   if (args && args[0])
      strncpy(g_socket_path, args, sizeof(g_socket_path));
   else
      strcpy(g_socket_path, DEFAULT_SOCKET_PATH);
   logger(LG_INFO, "Version %s initializing. Socket path: '%s'", VERSION, g_socket_path);
   logger(LOG_INFO, "HIRN: Nagios Version is %s", get_program_version());

   if (!open_unix_socket())
      return 1;

   store_init();
   register_callbacks();

   /* Unfortunately, we cannot start our socket thread right now.
      Nagios demonizes *after* having loaded the NEB modules. When
      demonizing we are losing our thread. Therefore, we create the
      thread the first time one of our callbacks is called. Before
      that happens, we haven't got any data anyway... */

   logger(LG_INFO, "successfully finished initialization");
   return 0;
}

int nebmodule_deinit(int flags, int reason)
{
   logger(LG_INFO, "deinitializing");
   terminate_threads();
   close_unix_socket();
   store_deinit();
   deregister_callbacks();
}

