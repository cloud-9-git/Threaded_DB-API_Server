#include "server_entry.h"

int main(int argc, char **argv)
{
    return server_entry_main(argc, argv,
                             SERVER_DISPATCH_SERIAL,
                             "mini_db_server_serial");
}
