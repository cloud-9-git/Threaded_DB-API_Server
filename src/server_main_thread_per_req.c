#include "server_entry.h"

int main(int argc, char **argv)
{
    return server_entry_main(argc, argv,
                             SERVER_DISPATCH_THREAD_PER_REQUEST,
                             "mini_db_server_thread_per_req");
}
