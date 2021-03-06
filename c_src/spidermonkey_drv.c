/* author Kevin Smith <ksmith@basho.com>
   copyright 2009-2010 Basho Technologies

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <string.h>
#include <erl_driver.h>

#include "spidermonkey.h"
#include "config.h"
#include "driver_comm.h"

typedef struct _spidermonkey_drv_t {
  ErlDrvPort port;
  spidermonkey_vm *vm;
  ErlDrvTermData atom_ok;
  ErlDrvTermData atom_error;
  ErlDrvTermData atom_unknown_cmd;
  int shutdown;
} spidermonkey_drv_t;

typedef struct _js_call_t {
  spidermonkey_drv_t *driver_data;
  ErlDrvBinary *args;
} js_call;

typedef void (*asyncfun)(void *);


/* Forward declarations */
static ErlDrvData start(ErlDrvPort port, char *cmd);
static int init();
static void stop(ErlDrvData handle);
static void process(ErlDrvData handle, ErlIOVec *ev);

static ErlDrvEntry spidermonkey_drv_entry = {
    init,                             /* init */
    start,                            /* startup */
    stop,                             /* shutdown */
    NULL,                             /* output */
    NULL,                             /* ready_input */
    NULL,                             /* ready_output */
    (char *) "erlang_js_drv",         /* the name of the driver */
    NULL,                             /* finish */
    NULL,                             /* handle */
    NULL,                             /* control */
    NULL,                             /* timeout */
    process,                          /* process */
    NULL,                             /* ready_async */
    NULL,                             /* flush */
    NULL,                             /* call */
    NULL,                             /* event */
    ERL_DRV_EXTENDED_MARKER,          /* ERL_DRV_EXTENDED_MARKER */
    ERL_DRV_EXTENDED_MAJOR_VERSION,   /* ERL_DRV_EXTENDED_MAJOR_VERSION */
    ERL_DRV_EXTENDED_MAJOR_VERSION,   /* ERL_DRV_EXTENDED_MINOR_VERSION */
    ERL_DRV_FLAG_USE_PORT_LOCKING     /* ERL_DRV_FLAGs */
};


void send_output(ErlDrvPort port, ErlDrvTermData *terms, int term_count) {
  driver_output_term(port, terms, term_count);
}

void send_ok_response(spidermonkey_drv_t *dd, const char *call_id) {
  ErlDrvTermData terms[] = {ERL_DRV_BUF2BINARY, (ErlDrvTermData) call_id, strlen(call_id),
			    ERL_DRV_ATOM, dd->atom_ok,
			    ERL_DRV_TUPLE, 2};
  send_output(dd->port, terms, sizeof(terms) / sizeof(terms[0]));
}

void send_error_string_response(spidermonkey_drv_t *dd, const char *call_id, const char *msg) {
  ErlDrvTermData terms[] = {ERL_DRV_BUF2BINARY, (ErlDrvTermData) call_id, strlen(call_id),
                            ERL_DRV_ATOM, dd->atom_error,
			    ERL_DRV_BUF2BINARY, (ErlDrvTermData) msg, strlen(msg),
			    ERL_DRV_TUPLE, 3};
  send_output(dd->port, terms, sizeof(terms) / sizeof(terms[0]));
}

void send_string_response(spidermonkey_drv_t *dd, const char *call_id, const char *result) {
  ErlDrvTermData terms[] = {ERL_DRV_BUF2BINARY, (ErlDrvTermData) call_id, strlen(call_id),
                            ERL_DRV_ATOM, dd->atom_ok,
			    ERL_DRV_BUF2BINARY, (ErlDrvTermData) result, strlen(result),
			    ERL_DRV_TUPLE, 3};
  send_output(dd->port, terms, sizeof(terms) / sizeof(terms[0]));
}

void unknown_command(spidermonkey_drv_t *dd, const char *call_id) {
  ErlDrvTermData terms[] = {ERL_DRV_BUF2BINARY, (ErlDrvTermData) call_id, strlen(call_id),
                            ERL_DRV_ATOM, dd->atom_error,
			    ERL_DRV_ATOM, dd->atom_unknown_cmd,
			    ERL_DRV_TUPLE, 3};
  send_output(dd->port, terms, sizeof(terms) / sizeof(terms[0]));
}

void run_js(void *jsargs) {
  js_call *call_data = (js_call *) jsargs;
  spidermonkey_drv_t *dd = call_data->driver_data;
  ErlDrvBinary *args = call_data->args;
  driver_free(call_data);
  char *data = args->orig_bytes;
  char *command = read_command(&data);
  char *call_id = read_string(&data);
  char *result = NULL;
  if (strncmp(command, "ej", 2) == 0) {
    char *filename = read_string(&data);
    char *code = read_string(&data);
    result = sm_eval(dd->vm, filename, code, 1);
    if (strstr(result, "{\"error\"") != NULL) {
      send_error_string_response(dd, call_id, result);
    }
    else {
      send_string_response(dd, call_id, result);
    }
    driver_free(filename);
    driver_free(code);
    driver_free(result);
  }
  else if (strncmp(command, "dj", 2) == 0) {
    char *filename = read_string(&data);
    char *code = read_string(&data);
    result = sm_eval(dd->vm, filename, code, 0);
    if (result == NULL) {
      send_ok_response(dd, call_id);
    }
    else {
      send_error_string_response(dd, call_id, result);
      driver_free(result);
    }
    driver_free(filename);
    driver_free(code);
  }
  else if (strncmp(command, "sd", 2) == 0) {
    dd->shutdown = 1;
    send_ok_response(dd, call_id);
  }
  else {
    unknown_command(dd, call_id);
  }
  driver_free(command);
  driver_free(call_id);
  driver_binary_dec_refc(args);
}

DRIVER_INIT(spidermonkey_drv) {
  return &spidermonkey_drv_entry;
}

static int init() {
  sm_configure_locale();
  return 0;
}

static ErlDrvData start(ErlDrvPort port, char *cmd) {
  spidermonkey_drv_t *retval = (spidermonkey_drv_t*) driver_alloc(sizeof(spidermonkey_drv_t));
  retval->port = port;
  retval->shutdown = 0;
  retval->atom_ok = driver_mk_atom((char *) "ok");
  retval->atom_error = driver_mk_atom((char *) "error");
  retval->atom_unknown_cmd = driver_mk_atom((char *) "unknown_command");
  return (ErlDrvData) retval;
}

static void stop(ErlDrvData handle) {
  spidermonkey_drv_t *dd = (spidermonkey_drv_t*) handle;
  sm_stop(dd->vm);
  if (dd->shutdown) {
    sm_shutdown();
  }
  driver_free(dd);
}

static void process(ErlDrvData handle, ErlIOVec *ev) {
  spidermonkey_drv_t *dd = (spidermonkey_drv_t *) handle;
  char *data = ev->binv[1]->orig_bytes;
  char *command = read_command(&data);
  if (strncmp(command, "ij", 2) == 0) {
    char *call_id = read_string(&data);
    int thread_stack = read_int32(&data);
    if (thread_stack < 8) {
      thread_stack = 8;
    }
    thread_stack = thread_stack * (1024 * 1024);
    int heap_size = read_int32(&data) * (1024 * 1024);
    dd->vm = sm_initialize(thread_stack, heap_size);
    send_ok_response(dd, call_id);
    driver_free(call_id);
    driver_free(command);
  }
  else {
    js_call *call_data = (js_call *) driver_alloc(sizeof(js_call));
    call_data->driver_data = dd;
    call_data->args = ev->binv[1];
    driver_binary_inc_refc(call_data->args);
    ErlDrvPort port = dd->port;
    unsigned long thread_key = (unsigned long) port;
    driver_async(dd->port, (unsigned int *) &thread_key, (asyncfun) run_js, (void *) call_data, NULL);
  }
}
