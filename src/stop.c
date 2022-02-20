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

#include "service_runner.h"

enum {
    OPT_STOP_PIDFILE,
    OPT_STOP_SHUTDOWN_TIMEOUT,
    OPT_STOP_COUNT,
};

const struct option stop_options[] = {
    [OPT_STOP_PIDFILE]          = { "pidfile", required_argument, 0, 'p' },
    [OPT_STOP_SHUTDOWN_TIMEOUT] = { "shutdown-timeout", required_argument, 0, 0 },
    [OPT_STOP_COUNT]            = { 0, 0, 0, 0 },
};

int command_stop(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    int longind = 0;

    const char *pidfile = NULL;
    unsigned int shutdown_timeout = 0;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:", stop_options, &longind);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 0:
                switch (longind) {
                    case OPT_STOP_SHUTDOWN_TIMEOUT:
                    {
                        char *endptr = NULL;
                        unsigned long value = strtoul(optarg, &endptr, 10);
                        if (!*optarg || *endptr || value > UINT_MAX) {
                            fprintf(stderr, "*** error: illegal value for --shutdown-timeout: %s\n", optarg);
                            return 1;
                        }
                        shutdown_timeout = value;
                        break;
                    }
                }
                break;

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

    pid_t runner_pid  = 0;
    pid_t service_pid = 0;
    bool pidfile_runner_ok = read_pidfile(pidfile_runner, &runner_pid) == 0;
    bool pidfile_ok        = read_pidfile(pidfile, &service_pid) == 0;

    pid_t pid = 0;
    if (pidfile_runner_ok) {
        if (kill(runner_pid, SIGTERM) != 0) {
            fprintf(stderr, "*** error: sending SIGTERM to service-runner PID %d: %s\n", runner_pid, strerror(errno));
            status = 1;
            goto cleanup;
        }

        fprintf(stderr, "will send SIGTERM to service-runner at PID %d\n", runner_pid);
        pid = runner_pid;
    } else if (pidfile_ok) {
        if (kill(service_pid, SIGTERM) != 0) {
            fprintf(stderr, "*** error: sending SIGTERM to service PID %d: %s\n", service_pid, strerror(errno));
            status = 1;
            goto cleanup;
        }

        fprintf(stderr, "will send SIGTERM to %s service at PID %d\n", name, service_pid);
        pid = service_pid;
    } else {
        fprintf(stderr, "*** error: %s is not running\n", name);
        goto cleanup;
    }

    struct timespec ts_before;
    struct timespec ts_after;
    bool time_ok = true;

    if (shutdown_timeout != 0 && clock_gettime(CLOCK_MONOTONIC, &ts_before) != 0) {
        time_ok = false;
        fprintf(stderr, "*** error: clock_gettime(CLOCK_MONOTONIC, &ts_before): %s\n", strerror(errno));
    }

    bool had_timeout = false;
    for (size_t count = 0; !had_timeout; ++ count) {
        if (kill(pid, 0) == 0) {
            if (shutdown_timeout != 0) {
                if (time_ok && clock_gettime(CLOCK_MONOTONIC, &ts_after) != 0) {
                    time_ok = false;
                    fprintf(stderr, "*** error: clock_gettime(CLOCK_MONOTONIC, &ts_after): %s\n", strerror(errno));
                }

                if (time_ok) {
                    time_t secs = ts_after.tv_sec - ts_before.tv_sec;
                    if (secs > shutdown_timeout) {
                        had_timeout = true;
                        break;
                    }
                } else if (count > shutdown_timeout) {
                    had_timeout = true;
                    break;
                }
            }
            sleep(1);
        } else if (errno == ESRCH) {
            break;
        } else {
            fprintf(stderr, "*** error: waiting for PID %d: %s\n", pid, strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    if (had_timeout) {
        fprintf(stderr, "*** error: timeout waiting for %s to shutdown, sending SIGKILL\n", name);
        status = 1;

        if (pidfile_ok) {
            if (kill(service_pid, SIGKILL) != 0) {
                fprintf(stderr, "*** error: sending SIGKILL to service PID %d\n", service_pid);
            }

            if (read_pidfile(pidfile, &pid) == 0 && pid == service_pid && unlink(pidfile) != 0 && errno != ENOENT) {
                fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile, strerror(errno));
            }
        }

        if (pidfile_runner_ok) {
            if (kill(runner_pid, SIGKILL) != 0) {
                fprintf(stderr, "*** error: sending SIGKILL to service-runner PID %d\n", runner_pid);
            }

            if (read_pidfile(pidfile_runner, &pid) == 0 && pid == runner_pid && unlink(pidfile_runner) != 0 && errno != ENOENT) {
                fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile_runner, strerror(errno));
            }
        }
    }

cleanup:
    if (free_pidfile) {
        free((char*)pidfile);
    }

    return status;
}
