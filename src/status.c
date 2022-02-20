#define _POSIX_C_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>

#include "service_runner.h"

enum {
    OPT_STATUS_PIDFILE,
    OPT_STATUS_COUNT,
};

const struct option status_options[] = {
    [OPT_STATUS_PIDFILE] = { "pidfile", required_argument, 0, 'p' },
    [OPT_STATUS_COUNT]   = { 0, 0, 0, 0 },
};

int command_status(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    const char *pidfile = NULL;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:", status_options, NULL);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 'p':
                pidfile = optarg;
                break;

            case '?':
                short_usage(argc, argv);
                return 1;
        }
    }

    // because of skipped first argument:
    ++ optind;

    int count = argc - optind;
    if (count != 1) {
        fprintf(stderr, "*** error: illegal number of arguments\n");
        short_usage(argc, argv);
        return 1;
    }

    const char *name = argv[optind ++];

    int status = 0;
    bool free_pidfile = false;
    char *pidfile_runner = NULL;

    switch (get_pidfile_abspath((char**)&pidfile, name)) {
        case ABS_PATH_NEW:
            free_pidfile = true;
            break;

        case ABS_PATH_ORIG:
            break;

        case ABS_PATH_ERR:
            status = 1;
            goto cleanup;
    }

    {
        size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
        pidfile_runner = malloc(pidfile_runner_size);
        if (pidfile_runner == NULL) {
            fprintf(stderr, "*** error: malloc: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        int count = snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile);
        assert(count >= 0 && (size_t)count == pidfile_runner_size - 1); (void)count;
    }

    pid_t runner_pid = 0;
    pid_t service_pid = 0;
    bool pidfile_runner_ok = read_pidfile(pidfile_runner, &runner_pid) == 0;
    bool pidfile_ok        = read_pidfile(pidfile, &service_pid) == 0;
    bool runner_pid_ok  = false;
    bool service_pid_ok = false;

    if (pidfile_runner_ok) {
        if (kill(runner_pid, 0) == 0) {
            runner_pid_ok = true;
        } else {
            status = 2;
            fprintf(stderr, "%s: error: pidfile %s exists, but service-runner PID %d does not\n", name, pidfile_runner, runner_pid);
        }
    }
    
    if (pidfile_ok) {
        if (kill(service_pid, 0) == 0) {
            service_pid_ok = true;
        } else {
            status = 3;
            fprintf(stderr, "%s: error: pidfile %s exists, but service PID %d does not\n", name, pidfile, service_pid);
        }
    }

    if (!pidfile_runner_ok && !pidfile_ok) {
        fprintf(stderr, "%s is not running\n", name);
        status = 1;
    } else if (pidfile_runner_ok && pidfile_ok && runner_pid_ok && service_pid_ok) {
        printf("%s is running\n", name);
    }

cleanup:
    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    return status;
}