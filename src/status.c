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

#include "service-runner.h"

enum {
    OPT_STATUS_PIDFILE,
    OPT_STATUS_COUNT,
};

static const struct option status_options[] = {
    [OPT_STATUS_PIDFILE] = { "pidfile", required_argument, 0, 'p' },
    [OPT_STATUS_COUNT]   = { 0, 0, 0, 0 },
};

int command_status(int argc, char *argv[]) {
    if (argc < 2) {
        return 150;
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
                return 150;
        }
    }

    // because of skipped first argument:
    ++ optind;

    int count = argc - optind;
    if (count != 1) {
        fprintf(stderr, "*** error: illegal number of arguments\n");
        short_usage(argc, argv);
        return 150;
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
            status = 150;
            goto cleanup;
    }

    {
        size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
        pidfile_runner = malloc(pidfile_runner_size);
        if (pidfile_runner == NULL) {
            fprintf(stderr, "*** error: malloc(%zu): %s\n", pidfile_runner_size, strerror(errno));
            status = 150;
            goto cleanup;
        }

        int count = snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile);
        assert(count >= 0 && (size_t)count == pidfile_runner_size - 1); (void)count;
    }

    pid_t runner_pid  = 0;
    pid_t service_pid = 0;
    bool pidfile_runner_ok = read_pidfile(pidfile_runner, &runner_pid) == 0;
    bool pidfile_ok        = read_pidfile(pidfile, &service_pid) == 0;
    bool runner_pid_ok  = false;
    bool service_pid_ok = false;

    if (pidfile_runner_ok) {
        errno = 0;
        if (kill(runner_pid, 0) == 0 || errno == EPERM) {
            runner_pid_ok = true;
        } else if (errno == ESRCH) {
            fprintf(stderr, "%s: error: service-runner pidfile %s exists, but PID %d does not\n", name, pidfile_runner, runner_pid);
        } else {
            fprintf(stderr, "*** error: kill(%d, 0): %s\n", runner_pid, strerror(errno));
            status = 150;
            goto cleanup;
        }
    }

    if (pidfile_ok) {
        errno = 0;
        if (kill(service_pid, 0) == 0 || errno == EPERM) {
            service_pid_ok = true;
        } else if (errno == ESRCH) {
            fprintf(stderr, "%s: error: service pidfile %s exists, but PID %d does not\n", name, pidfile, service_pid);
        } else {
            fprintf(stderr, "*** error: kill(%d, 0): %s\n", runner_pid, strerror(errno));
            status = 150;
            goto cleanup;
        }
    }

    // Trying to map LSB service status exit codes to what I do here.
    // https://refspecs.linuxbase.org/LSB_3.0.0/LSB-PDA/LSB-PDA/iniscrptact.html
    if (!runner_pid_ok && !service_pid_ok) {
        fprintf(stderr, "%s is not running\n", name);
        status = pidfile_ok || pidfile_runner_ok ? 1 : 3;
    } else if (runner_pid_ok && service_pid_ok) {
        printf("%s is running\n", name);
        status = 0;
    } else if (!runner_pid_ok && service_pid_ok) {
        fprintf(stderr, "%s is running, but it's service-runner is not\n", name);
        status = 1; // maybe?
    } else if (runner_pid_ok && !service_pid_ok) {
        fprintf(stderr, "%s is not running, but it's service-runner is.\n", name);
        fprintf(stderr, "This means the service is probably currently (re)starting.\n");
        status = pidfile_ok ? 1 : 4; // maybe?
    }

cleanup:
    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    return status;
}
