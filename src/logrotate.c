#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <poll.h>

#include "service-runner.h"

enum {
    OPT_LOGROTATE_PIDFILE,
    OPT_LOGROTATE_COUNT,
};

static const struct option restart_options[] = {
    [OPT_LOGROTATE_PIDFILE] = { "pidfile", required_argument, 0, 'p' },
    [OPT_LOGROTATE_COUNT]   = { 0, 0, 0, 0 },
};

int command_logrotate(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    int longind = 0;

    const char *pidfile = NULL;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:", restart_options, &longind);

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
    bool pidfile_runner_ok = read_pidfile(pidfile_runner, &runner_pid) == 0;

    if (!pidfile_runner_ok) {
        fprintf(stderr, "*** error: reading pidfile: %s: %s\n", pidfile_runner, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (kill(runner_pid, SIGHUP) != 0) {
        fprintf(stderr, "*** error: sending SIGHUP to service runner PID %d: %s\n", runner_pid, strerror(errno));
        status = 1;
        goto cleanup;
    }

cleanup:
    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    return status;
}
