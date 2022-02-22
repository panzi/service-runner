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
    int shutdown_timeout = -1;

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
                        long value = strtol(optarg, &endptr, 10);
                        if (!*optarg || *endptr || value > (INT_MAX / 1000) || value < -1) {
                            fprintf(stderr, "*** error: illegal value for --shutdown-timeout: %s\n", optarg);
                            return 1;
                        }
                        shutdown_timeout = value == -1 ? -1 : value * 1000;
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
    int pidfd = -1;
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
    const char *which = NULL;
    if (pidfile_runner_ok) {
        pid = runner_pid;
        which = "service-runner";
    } else if (pidfile_ok) {
        pid = service_pid;
        which = name;
    } else {
        fprintf(stderr, "*** error: %s is not running\n", name);
        goto cleanup;
    }

    pidfd = pidfd_open(pid, 0);
    if (pidfd == -1 && errno == ENOSYS) {
        // fallback without pidfd support
        // weird that pidfd works in start, though?
        fprintf(stderr, "*** error: opening %s PID %d as pidfd no supported, falling back to kill polling...\n", which, pid);

        struct timespec ts_before = {
            .tv_sec  = 0,
            .tv_nsec = 0,
        };

        if (shutdown_timeout >= 0 && clock_gettime(CLOCK_MONOTONIC, &ts_before) != 0) {
            fprintf(stderr, "*** error: clock_gettime(CLOCK_MONOTONIC, &ts_before): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (kill(pid, SIGTERM) != 0) {
            fprintf(stderr, "*** error: sending SIGTERM to %s PID %d: %s\n", which, pid, strerror(errno));
            status = 1;
            goto cleanup;
        }

        for (;;) {
            struct timespec wait_time = {
                .tv_sec  = 0,
                .tv_nsec = 500000000,
            };
            nanosleep(&wait_time, NULL);

            if (kill(pid, 0) != 0) {
                if (errno == ESRCH) {
                    break;
                }
                fprintf(stderr, "*** error: sending 0 to %s PID %d: %s\n", which, pid, strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (shutdown_timeout >= 0) {
                struct timespec ts_after = {
                    .tv_sec  = 0,
                    .tv_nsec = 0,
                };

                if (clock_gettime(CLOCK_MONOTONIC, &ts_after) != 0) {
                    fprintf(stderr, "*** error: clock_gettime(CLOCK_MONOTONIC, &ts_after): %s\n", strerror(errno));
                    status = 1;
                    goto cleanup;
                }

                if (ts_after.tv_sec - ts_before.tv_sec > shutdown_timeout) {
                    // timeout
                    fprintf(stderr, "*** error: timeout waiting for %s to shutdown, sending SIGKILL...\n", name);

                    if (pidfile_ok) {
                        if (kill(service_pid, SIGKILL) != 0 && errno != ESRCH) {
                            fprintf(stderr, "*** error: sending SIGKILL to service PID %d\n", service_pid);
                        }

                        if (read_pidfile(pidfile, &pid) == 0 && pid == service_pid && unlink(pidfile) != 0 && errno != ENOENT) {
                            fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile, strerror(errno));
                        }
                    }

                    if (pidfile_runner_ok) {
                        if (kill(runner_pid, SIGKILL) != 0 && errno != ESRCH) {
                            fprintf(stderr, "*** error: sending SIGKILL to service-runner PID %d\n", runner_pid);
                        }

                        if (read_pidfile(pidfile_runner, &pid) == 0 && pid == runner_pid && unlink(pidfile_runner) != 0 && errno != ENOENT) {
                            fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile_runner, strerror(errno));
                        }
                    }

                    status = 1;
                    goto cleanup;
                }
            }
        }

        goto cleanup;
    }

    // pidfd is supported. use pidfd_send_signal() and poll() with its timeout mechanism
    if (pidfd == -1) {
        fprintf(stderr, "*** error: opening %s PID %d as pidfd: %s\n", which, pid, strerror(errno));
        status = 1;
        goto cleanup;
    }

    printf("Sending SIGTERM to %s at PID %d...\n", which, pid);
    if (pidfd_send_signal(pidfd, SIGTERM, NULL, 0) != 0) {
        if (errno == EBADFD || errno == ENOSYS) {
            fprintf(stderr, "*** error: pidfd_send_signal(pidfd, SIGTERM, NULL, 0) failed, using kill(%d, SIGTERM): %s\n",
                pid, strerror(errno));
            if (kill(pid, SIGTERM) != 0) {
                fprintf(stderr, "*** error: sending SIGTERM to %s PID %d: %s\n", which, pid, strerror(errno));
                status = 1;
                goto cleanup;
            }
        } else {
            fprintf(stderr, "*** error: pidfd_send_signal(pidfd, SIGTERM, NULL, 0): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    struct pollfd pollfds[] = {
        { .fd = pidfd, .events = POLLIN, .revents = 0 },
    };

    int result = poll(pollfds, 1, shutdown_timeout);
    if (result == -1) {
        fprintf(stderr, "*** error: waiting for %s PID %d: %s\n", which, pid, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (result == 0) {
        // timeout
        fprintf(stderr, "*** error: timeout waiting for %s to shutdown, sending SIGKILL...\n", name);

        if (pidfile_ok) {
            if (kill(service_pid, SIGKILL) != 0 && errno != ESRCH) {
                fprintf(stderr, "*** error: sending SIGKILL to service PID %d\n", service_pid);
            }

            if (read_pidfile(pidfile, &pid) == 0 && pid == service_pid && unlink(pidfile) != 0 && errno != ENOENT) {
                fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile, strerror(errno));
            }
        }

        if (pidfile_runner_ok) {
            if (kill(runner_pid, SIGKILL) != 0 && errno != ESRCH) {
                fprintf(stderr, "*** error: sending SIGKILL to service-runner PID %d\n", runner_pid);
            }

            if (read_pidfile(pidfile_runner, &pid) == 0 && pid == runner_pid && unlink(pidfile_runner) != 0 && errno != ENOENT) {
                fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile_runner, strerror(errno));
            }
        }

        status = 1;
        goto cleanup;
    }

    if (pollfds[0].revents & POLLERR) {
        fprintf(stderr, "*** error: waiting for %s PID %d: POLLERR\n", which, pid);
        status = 1;
        goto cleanup;
    }

    if (kill(pid, 0) == 0) {
        // Process might be a zombie here.
        // Sleep half a second to give parent of the process or init
        // one more chance to wait for it.
        struct timespec wait_time = {
            .tv_sec  = 0,
            .tv_nsec = 500000000,
        };
        nanosleep(&wait_time, NULL);

        if (kill(pid, 0) == 0) {
            fprintf(stderr, "*** error: waiting for %s PID %d: poll() on pidfd returned successful, but process is still running\n", which, pid);
            fprintf(stderr, "This might mean the process is a zombie and not yet waited for by it's parent/init, maybe because of heavy system load?\n");
            status = 1;
            goto cleanup;
        }
    }

    if (errno != ESRCH) {
        fprintf(stderr, "*** error: waiting for %s PID %d: %s\n", which, pid, strerror(errno));
        status = 1;
        goto cleanup;
    }

cleanup:
    if (pidfd != -1) {
        close(pidfd);
    }

    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    return status;
}
