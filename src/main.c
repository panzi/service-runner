#include <stdio.h>
#include <string.h>

#include "service_runner.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "*** error: not enough arguments\n");
        usage(argc, argv);
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "start") == 0) {
        return command_start(argc, argv);
    } else if (strcmp(command, "stop") == 0) {
        return command_stop(argc, argv);
    } else if (strcmp(command, "status") == 0) {
        return command_status(argc, argv);
    } else if (strcmp(command, "help") == 0) {
        return command_help(argc, argv);
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        usage(argc, argv);
        return 1;
    }

    return 0;
}
