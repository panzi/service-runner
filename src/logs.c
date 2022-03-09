#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <stdalign.h>

#include "service-runner.h"

enum {
    OPT_LOGS_PIDFILE,
    OPT_LOGS_FOLLOW,
    OPT_LOGS_COUNT,
};

static const struct option logs_options[] = {
    [OPT_LOGS_PIDFILE] = { "pidfile", required_argument, 0, 'p' },
    [OPT_LOGS_FOLLOW]  = { "follow",  no_argument,       0, 'f' },
    [OPT_LOGS_COUNT]   = { 0, 0, 0, 0 },
};

int command_logs(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    const char *pidfile = NULL;
    bool follow = false;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:f", logs_options, NULL);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 'p':
                pidfile = optarg;
                break;

            case 'f':
                follow = true;
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
    int logfile_fd = -1;
    int inotify_fd = -1;
    int procdir_wd = -1;
    int stdout_wd  = -1;

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
            fprintf(stderr, "*** error: malloc(%zu): %s\n", pidfile_runner_size, strerror(errno));
            status = 1;
            goto cleanup;
        }

        int count = snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile);
        assert(count >= 0 && (size_t)count == pidfile_runner_size - 1); (void)count;
    }

    pid_t runner_pid = 0;
    if (read_pidfile(pidfile_runner, &runner_pid) != 0) {
        fprintf(stderr, "*** error: reading pidfile %s: %s\n", pidfile_runner, strerror(errno));
        status = 1;
        goto cleanup;
    }

    char runner_stdout [32];
    char runner_procdir[32];
    {
        ssize_t count = snprintf(runner_stdout, sizeof(runner_stdout), "/proc/%d/fd/1", runner_pid);
        if (count >= sizeof(runner_stdout)) {
            errno = ENAMETOOLONG;
            count = -1;
        }

        if (count < 0) {
            fprintf(stderr, "*** error: opening service-runner stdout: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        count = snprintf(runner_procdir, sizeof(runner_procdir), "/proc/%d/fd", runner_pid);
        if (count >= sizeof(runner_procdir)) {
            errno = ENAMETOOLONG;
            count = -1;
        }

        if (count < 0) {
            fprintf(stderr, "*** error: generating service-runner proc dir path: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    struct stat logfile_meta = {
        .st_dev = 0,
        .st_ino = 0,
    };
    if (follow) {
        inotify_fd = inotify_init1(IN_CLOEXEC);
        if (inotify_fd == -1) {
            fprintf(stderr, "*** error: opening inotify file descriptor: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        // IN_ATTRIB for unlink

        procdir_wd = inotify_add_watch(inotify_fd, runner_procdir, IN_CREATE | IN_ATTRIB);
        if (procdir_wd == -1) {
            fprintf(stderr, "*** error: watching %s: %s\n", runner_procdir, strerror(errno));
            status = 1;
            goto cleanup;
        }

        stdout_wd = inotify_add_watch(inotify_fd, runner_stdout, IN_MODIFY | IN_CLOSE_WRITE);
        if (stdout_wd == -1 && errno != ENOENT) {
            fprintf(stderr, "*** error: watching %s: %s\n", runner_stdout, strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    bool modified = true;
    bool closed   = false;
    bool pidgone  = false;

    for (;;) {
        if (modified) {
            if (logfile_fd == -1) {
                logfile_fd = open(runner_stdout, O_CLOEXEC | O_RDONLY);

                if (logfile_fd == -1 && (!follow || errno != ENOENT)) {
                    fprintf(stderr, "*** error: opening service-runner stdout %s: %s\n", runner_stdout, strerror(errno));
                    status = 1;
                    goto cleanup;
                }

                if (logfile_fd != -1 && fstat(logfile_fd, &logfile_meta) != 0) {
                    fprintf(stderr, "*** error: reading meta-data of service-runner stdout %s: %s\n", runner_stdout, strerror(errno));
                    status = 1;
                    goto cleanup;
                }
            }

            if (logfile_fd != -1) {
                char buf[BUFSIZ];

                for (;;) {
                    ssize_t count = read(logfile_fd, buf, sizeof(buf));

                    if (count < 0) {
                        fprintf(stderr, "*** error: reading logs: %s\n", strerror(errno));
                        status = 1;
                        goto cleanup;
                    }

                    if (count == 0) {
                        break;
                    }

                    fwrite(buf, count, 1, stdout);
                }

                fflush(stdout);

                if (closed) {
                    close(logfile_fd);
                    logfile_fd = -1;
                    closed = false;
                }
            }

            modified = false;
        }

        if (!follow) {
            break;
        }

        if (!pidgone) {
            struct pollfd pollfds[1] = {
                { .fd = inotify_fd, .events = POLLIN, .revents = 0 },
            };

            int count = poll(pollfds, 1, -1);
            if (count < 0) {
                fprintf(stderr, "*** error: polling for inotify events: %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (pollfds[0].revents & POLLERR) {
                fprintf(stderr, "*** error: polling for inotify events: %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (pollfds[0].revents & POLLIN) {
                alignas(alignof(struct inotify_event)) char buf[4096];
                ssize_t count = read(inotify_fd, buf, sizeof(buf));
                if (count < 0) {
                    fprintf(stderr, "*** error: polling for inotify events: %s\n", strerror(errno));
                    status = 1;
                    goto cleanup;
                }

                const char *endptr = buf + count;
                const char *ptr = buf;
                if (ptr < endptr) {
                    bool procdir_attrib = false;
                    bool procdir_create = false;
                    bool stdout_close   = false;

                    for (;;) {
                        struct inotify_event *event = (struct inotify_event*) ptr;

                        if (stdout_wd != -1 && event->wd == stdout_wd) {
                            if (event->mask & IN_MODIFY) {
                                modified = true;
                            }

                            if (event->mask & IN_CLOSE_WRITE) {
                                stdout_close = true;
                            }
                        } else if (event->wd == procdir_wd) {
                            if (event->mask & IN_ATTRIB) {
                                procdir_attrib = true;
                            }

                            if (event->mask & IN_CREATE) {
                                procdir_create = true;
                            }
                        }

                        ptr += sizeof(struct inotify_event) + event->len;
                        if (ptr >= endptr) {
                            break;
                        }
                    }

                    bool rewatch = false;

                    if (procdir_attrib) {
                        struct stat meta;
                        if (stat(runner_procdir, &meta) != 0) {
                            if (errno == ENOENT) {
                                pidgone = true;
                            }
                        }
                    }

                    if (procdir_create && stdout_wd == -1) {
                        rewatch = true;
                    }

                    if (stdout_close && !closed) {
                        struct stat meta;
                        if (stat(runner_stdout, &meta) != 0) {
                            if (errno == ENOENT) {
                                closed = rewatch = true;
                            }
                        } else if (meta.st_dev != logfile_meta.st_dev || meta.st_ino != logfile_meta.st_ino) {
                            closed = rewatch = true;
                        }
                    }

                    if (rewatch) {
                        if (stdout_wd != -1 && inotify_rm_watch(inotify_fd, stdout_wd) != 0) {
                            fprintf(stderr, "*** error: watching %s: %s\n", runner_stdout, strerror(errno));
                            status = 1;
                            goto cleanup;
                        }

                        stdout_wd = inotify_add_watch(inotify_fd, runner_stdout, IN_MODIFY | IN_CLOSE_WRITE);
                        if (stdout_wd == -1 && errno != ENOENT) {
                            fprintf(stderr, "*** error: watching %s: %s\n", runner_stdout, strerror(errno));
                            status = 1;
                            goto cleanup;
                        }
                    }
                }
            }
        }

        if (pidgone && !modified) {
            break;
        }
    }

cleanup:
    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    if (logfile_fd != -1) {
        close(logfile_fd);
    }

    if (inotify_fd != -1) {
        close(inotify_fd);
    }

    return status;
}
