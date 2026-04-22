#ifndef SERVER_ENTRY_H
#define SERVER_ENTRY_H

#include "server.h"

int server_entry_main(int argc,
                      char **argv,
                      ServerDispatchMode dispatch_mode,
                      const char *service_name);

#endif
