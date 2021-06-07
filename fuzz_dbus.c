#include <stdio.h>
#include <stdlib.h>
#include <dbus/dbus.h>
#include <assert.h>

/*
   american fuzzy lop - sample argv fuzzing wrapper
   ------------------------------------------------
   Written by Michal Zalewski <lcamtuf@google.com>
   Copyright 2015 Google Inc. All rights reserved.
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:
     http://www.apache.org/licenses/LICENSE-2.0
   This file shows a simple way to fuzz command-line parameters with stock
   afl-fuzz. To use, add:
   #include "/path/to/argv-fuzz-inl.h"
   ...to the file containing main(), ideally placing it after all the 
   standard includes. Next, put AFL_INIT_ARGV(); near the very beginning of
   main().
   This will cause the program to read NUL-delimited input from stdin and
   put it in argv[]. Two subsequent NULs terminate the array. Empty
   params are encoded as a lone 0x02. Lone 0x02 can't be generated, but
   that shouldn't matter in real life.
   If you would like to always preserve argv[0], use this instead:
   AFL_INIT_SET0("prog_name");
*/

#ifndef _HAVE_ARGV_FUZZ_INL
#define _HAVE_ARGV_FUZZ_INL

#include <unistd.h>

#define AFL_INIT_ARGV() do { argv = afl_init_argv(&argc); } while (0)

#define AFL_INIT_SET0(_p) do { \
    argv = afl_init_argv(&argc); \
    argv[0] = (_p); \
    if (!argc) argc = 1; \
  } while (0)

#define MAX_CMDLINE_LEN 100000
#define MAX_CMDLINE_PAR 1000

static char** afl_init_argv(int* argc) {

  static char  in_buf[MAX_CMDLINE_LEN];
  static char* ret[MAX_CMDLINE_PAR];

  char* ptr = in_buf;
  int   rc  = 1;

  if (read(0, in_buf, MAX_CMDLINE_LEN - 2) < 0)
  ;

  while (*ptr) {

    ret[rc] = ptr;
    if (ret[rc][0] == 0x02 && !ret[rc][1]) ret[rc]++;
    rc++;

    while (*ptr) ptr++;
    ptr++;

  }

  *argc = rc;

  return ret;

}

#undef MAX_CMDLINE_LEN
#undef MAX_CMDLINE_PAR

#endif /* !_HAVE_ARGV_FUZZ_INL */

DBusConnection* conn = NULL;

//Helper function to setup connection
void vsetupconnection();

//Send method call, Returns NULL on failure, else pointer to reply
DBusMessage* sendMethodCall(const char* objectpath, \
        const char* busname, \
        const char* interfacename, \
        const char* methodname);

// dbus-send --dest=im.pidgin.purple.PurpleService --print-reply --type=method_call /im/pidgin/purple/PurpleObject im.pidgin.purple.PurpleInterface.$METHOD_NAME "$@"

#define TEST_BUS_NAME               "im.pidgin.purple.PurpleService"
#define TEST_OBJ_PATH               "/im/pidgin/purple/PurpleObject"
#define TEST_INTERFACE_NAME         "im.pidgin.purple.PurpleInterface.PurpleAccountsGetAll"
#define TEST_METHOD_NAME            "PurpleAccountsGetAll"

int main (int argc, char **argv)
{
	AFL_INIT_ARGV();

	if (argc < 5) {
		printf("im.pidgin.purple.PurpleService /im/pidgin/purple/PurpleObject im.pidgin.purple.PurpleInterface.PurpleAccountsGetAll PurpleAccountsGetAll");
		return 1;
	}


    vsetupconnection();

    // TEST_OBJ_PATH, TEST_BUS_NAME, TEST_INTERFACE_NAME, TEST_METHOD_NAME
    DBusMessage* reply = sendMethodCall(argv[2], argv[1], argv[3], argv[4]);
    if(reply != NULL)    {

        DBusMessageIter MsgIter;
        dbus_message_iter_init(reply, &MsgIter);//msg is pointer to dbus message received

        if (DBUS_TYPE_STRING == dbus_message_iter_get_arg_type(&MsgIter)){
            char* str = NULL;
            dbus_message_iter_get_basic(&MsgIter, &str);
            printf("Received string: \n %s \n",str);
        }

        dbus_message_unref(reply);//unref reply
    }
    // dbus_connection_close(conn);
    return 0;
}

void vsetupconnection()
{
   DBusError err;
   // initialise the errors
   dbus_error_init(&err);
   // connect to session bus
   conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
   if (dbus_error_is_set(&err)) {
      printf("Connection Error (%s)\n", err.message);
      dbus_error_free(&err);
   }
   if (NULL == conn) {
      exit(1);
   }
   else   {
       printf("Connected to session bus\n");
   }
}

DBusMessage* sendMethodCall(const char* objectpath, const char* busname, const char* interfacename, const char* methodname)
{
    assert(objectpath != NULL); assert(busname != NULL);    assert(interfacename != NULL);
    assert(methodname != NULL); assert(conn != NULL);

    DBusMessage* methodcall = dbus_message_new_method_call(busname,objectpath, interfacename, methodname);

    if (methodcall == NULL)    {
        printf("Cannot allocate DBus message!\n");
    }
    //Now do a sync call
    DBusPendingCall* pending;
    DBusMessage* reply;

    if (!dbus_connection_send_with_reply(conn, methodcall, &pending, -1))//Send and expect reply using pending call object
    {
        printf("failed to send message!\n");
    }
    dbus_connection_flush(conn);
    dbus_message_unref(methodcall);
    methodcall = NULL;

    dbus_pending_call_block(pending);//Now block on the pending call
    reply = dbus_pending_call_steal_reply(pending);//Get the reply message from the queue
    dbus_pending_call_unref(pending);//Free pending call handle
    assert(reply != NULL);

    if(dbus_message_get_type(reply) ==  DBUS_MESSAGE_TYPE_ERROR)    {
        printf("Error : %s",dbus_message_get_error_name(reply));
            dbus_message_unref(reply);
            reply = NULL;
    }

    return reply;
}
