#include <stdio.h>
#include <string.h>

#include "service_runner.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "*** error: not enough arguments\n");
        short_usage(argc, argv);
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
    } else if (strcmp(command, "version") == 0) {
        if (argc != 2) {
            fprintf(stderr, "*** error: illegal number of arguments\n");
            short_usage(argc, argv);
            return 1;
        }
        printf("%d.%d.%d\n",
            SERVICE_RUNNER_VERSION_MAJOR,
            SERVICE_RUNNER_VERSION_MINOR,
            SERVICE_RUNNER_VERSION_PATCH);
        return 0;
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        short_usage(argc, argv);
        return 1;
    }

    return 0;
}
