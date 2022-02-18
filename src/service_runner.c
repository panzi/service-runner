#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wait.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <spawn.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>
#include <assert.h>
#include <poll.h>

extern char **environ;

void usage(int argc, char *argv[]) {
    const char *progname = argc > 0 ? argv[0] : "service_runner";
    printf("Usage: %s start  <name> [options] [--] <command> [argument...]\n", progname);
    printf("       %s stop   <name> [options]\n", progname);
    printf("       %s status <name> [options]\n", progname);
    printf("       %s help\n", progname);
    printf(
        "\n"
        "OPTIONS:\n"
        "       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: /var/run/NAME.pid\n"
        "       --logfile=FILE, -l FILE         Write service output to FILE. default: /var/log/NAME-%%Y-%%m-%%d.log\n"
        "       --user=USER, -u USER            Run service as USER (name or UID).\n"
        "       --group=GROUP, -g GROUP         Run service as GROUP (name or GID).\n"
        "       --crash-sleep=SECONDS           Wait SECONDS before restarting service. default: 1\n"
        "       --crash-report=COMMAND          Run `COMMAND $EXIT_STATUS $LOGFILE` if the service exited with a non-zero exit status.\n"
        "       --shutdown-timeout=SECONDS      If the service doesn't shut down after SECONDS after sending SIGTERM send SIGKILL. 0 means don't ever send SIGKILL. default: 0\n"
    );
}

void short_usage(int argc, char *argv[]) {
    const char *progname = argc > 0 ? argv[0] : "service_runner";
    fprintf(stderr, "for more info: %s help\n", progname);
}

int write_pidfile(const char *pidfile, pid_t pid) {
    FILE *fp = fopen(pidfile, "w");
    if (fp == NULL) {
        return -1;
    }

    if (fprintf(fp, "%d\n", pid) < 0) {
        fclose(fp);
        return -1;
    }

    return fclose(fp) == 0 ? 0 : -1;
}

int read_pidfile(const char *pidfile, pid_t *pidptr) {
    int value = 0;

    FILE *fp = fopen(pidfile, "r");
    if (fp == NULL) {
        return -1;
    }

    if (fscanf(fp, "%d", &value) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (pidptr != NULL) {
        *pidptr = value;
    }

    return 0;
}

enum {
    OPT_PIDFILE,
    OPT_LOGFILE,
    OPT_USER,
    OPT_GROUP,
    OPT_CRASH_REPORT,
    OPT_CRASH_SLEEP,
    OPT_SHUTDOWN_TIMEOUT,
    OPT_COUNT,
};

bool is_valid_name(const char *name) {
    if (!*name) {
        return false;
    }

    for (const char *ptr = name; *ptr; ++ ptr) {
        char ch = *ptr;
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' ||
              ch == '-' ||
              ch == '+')
        ) {
            return false;
        }
    }

    return true;
}

bool is_time_format_ok(const char *format) {
    // TODO: some formats have variable length (month names, day names)
    char buf[BUFSIZ];
    time_t ts = time(NULL);
    struct tm localts;
    localtime_r(&ts, &localts);
    return strftime(buf, sizeof(buf), format, &localts) != 0;
}

bool can_execute(const char *filename, uid_t uid, gid_t gid) {
    struct stat meta;

    if (stat(filename, &meta) != 0) {
        return false;
    }

    if (meta.st_mode & S_IXOTH) {
        return true;
    }

    if (meta.st_gid == gid && meta.st_mode & S_IXGRP) {
        return true;
    }

    if (meta.st_uid == uid && meta.st_mode & S_IXUSR) {
        return true;
    }

    return false;
}

bool can_read_write(const char *filename, uid_t uid, gid_t gid) {
    struct stat meta;

    if (stat(filename, &meta) != 0) {
        if (errno == ENOENT) {
            char *namedup = strdup(filename);
            if (namedup == NULL) {
                perror("strdup");
                return false;
            }

            const char *parent = dirname(namedup);
            if (stat(parent, &meta) != 0) {
                return false;
            }

            free(namedup);

            if (meta.st_mode & S_IWOTH) {
                return true;
            }

            if (meta.st_gid == gid && meta.st_mode & S_IWGRP) {
                return true;
            }

            if (meta.st_uid == uid && meta.st_mode & S_IWUSR) {
                return true;
            }

            return false;
        } else {
            return false;
        }
    }

    bool can_read  = false;
    bool can_write = false;

    if (meta.st_mode & S_IROTH) {
        can_read = true;
    }

    if (meta.st_gid == gid && meta.st_mode & S_IRGRP) {
        can_read = true;
    }

    if (meta.st_uid == uid && meta.st_mode & S_IRUSR) {
        can_read = true;
    }

    if (meta.st_mode & S_IWOTH) {
        can_write = true;
    }

    if (meta.st_gid == gid && meta.st_mode & S_IWGRP) {
        can_write = true;
    }

    if (meta.st_uid == uid && meta.st_mode & S_IWUSR) {
        can_write = true;
    }

    return can_read && can_write;
}

int get_uid_from_name(const char *username, uid_t *uidptr) {
    if (*username) {
        errno = EINVAL;
        return -1;
    }

    struct passwd pwd;
    struct passwd *result = NULL;

    size_t bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) {
        bufsize = 16384;
    }

    char *buf = malloc(bufsize);
    if (buf == NULL) {
        perror("malloc");
        return -1;
    }

    char *endptr = NULL;
    unsigned long uluid = strtoul(username, &endptr, 10);
    if (!*endptr && uluid <= UINT_MAX) {
        int status = getpwuid_r(uluid, &pwd, buf, bufsize, &result);
        if (result == NULL) {
            if (status == 0) {
                errno = ENOENT;
            } else {
                errno = status;
            }
            free(buf);
            return -1;
        }

        if (uidptr != NULL) {
            *uidptr = pwd.pw_uid;
        }
        free(buf);

        return 0;
    }

    int status = getpwnam_r(username, &pwd, buf, bufsize, &result);
    if (result == NULL) {
        if (status == 0) {
            errno = ENOENT;
        } else {
            errno = status;
        }
        free(buf);
        return -1;
    }

    if (uidptr != NULL) {
        *uidptr = pwd.pw_uid;
    }
    free(buf);

    return 0;
}

int get_gid_from_name(const char *groupname, gid_t *gidptr) {
    if (*groupname) {
        errno = EINVAL;
        return -1;
    }

    struct group grp;
    struct group *result = NULL;

    size_t bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (bufsize == -1) {
        bufsize = 16384;
    }

    char *buf = malloc(bufsize);
    if (buf == NULL) {
        perror("malloc");
        return -1;
    }

    char *endptr = NULL;
    unsigned long uluid = strtoul(groupname, &endptr, 10);
    if (!*endptr && uluid <= UINT_MAX) {
        int status = getgrgid_r(uluid, &grp, buf, bufsize, &result);
        if (result == NULL) {
            if (status == 0) {
                errno = ENOENT;
            } else {
                errno = status;
            }
            free(buf);
            return -1;
        }

        if (gidptr != NULL) {
            *gidptr = grp.gr_gid;
        }
        free(buf);

        return 0;
    }

    int status = getgrnam_r(groupname, &grp, buf, bufsize, &result);
    if (result == NULL) {
        if (status == 0) {
            errno = ENOENT;
        } else {
            errno = status;
        }
        free(buf);
        return -1;
    }

    if (gidptr != NULL) {
        *gidptr = grp.gr_gid;
    }
    free(buf);

    return 0;
}

const struct option start_options[] = {
    [OPT_PIDFILE]          = { "pidfile", required_argument, 0, 'p' },
    [OPT_LOGFILE]          = { "logfile", required_argument, 0, 'l' },
    [OPT_USER]             = { "user",    required_argument, 0, 'u' },
    [OPT_GROUP]            = { "group",   required_argument, 0, 'g' },
    [OPT_CRASH_REPORT]     = { "crash-report",     required_argument, 0, 0 },
    [OPT_CRASH_SLEEP]      = { "crash-sleep",      required_argument, 0, 0 },
    [OPT_SHUTDOWN_TIMEOUT] = { "shutdown-timeout", required_argument, 0, 0 },
    [OPT_COUNT]            = { 0, 0, 0, 0 },
};

pid_t service_pid = 0;

void forward_signal(int sig) {
    if (service_pid != 0) {
        if (kill(service_pid, sig) != 0) {
            fprintf(stderr, "*** error: forwarding signal %d to PID %d: %s\n", sig, service_pid, strerror(errno));
        }
    }
}

int command_start(int argc, char *argv[]) {
    if (argc < 1) {
        return 1;
    }

    int longind = 0;

    const char *pidfile = NULL;
    const char *logfile = NULL;
    const char *user    = NULL;
    const char *group   = NULL;

    const char *crash_report = NULL;
    unsigned int crash_sleep      = 1;
    unsigned int shutdown_timeout = 0;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:l:u:g:", start_options, &longind);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 0:
                switch (longind) {
                    case OPT_CRASH_REPORT:
                        crash_report = optarg;
                        break;

                    case OPT_CRASH_SLEEP:
                    {
                        char *endptr = NULL;
                        unsigned long value = strtoul(optarg, &endptr, 10);
                        if (!*optarg || *endptr || value > UINT_MAX) {
                            fprintf(stderr, "*** error: illegal value for --crash-sleep: %s\n", optarg);
                            return 1;
                        }
                        crash_sleep = value;
                        break;
                    }

                    case OPT_SHUTDOWN_TIMEOUT:
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

            case 'u':
                user = optarg;
                break;

            case 'g':
                group = optarg;
                break;

            case 'l':
                logfile = optarg;
                break;

            case '?':
                short_usage(argc, argv);
                return 1;
        }
    }

    int count = argc - optind;
    if (count < 2) {
        fprintf(stderr, "*** error: not enough arguments\n");
        short_usage(argc, argv);
        return 1;
    }

    const char *name = argv[optind ++];
    const char *command = argv[optind];
    char **command_argv = argv + optind;

    if (!is_valid_name(name)) {
        fprintf(stderr, "*** error: illegal name: '%s'\n", name);
        short_usage(argc, argv);
        return 1;
    }

    uid_t uid = 0;
    gid_t gid = 0;

    if (user != NULL && get_uid_from_name(user, &uid) != 0) {
        fprintf(stderr, "*** error: getting user ID for: %s\n", user);
        return 0;
    }

    if (group != NULL && get_gid_from_name(group, &gid) != 0) {
        fprintf(stderr, "*** error: getting group ID for: %s\n", group);
        return 0;
    }

    uid_t selfuid = getuid();
    uid_t selfgid = getgid();

    uid_t xuid = uid == 0 ? selfuid : uid;
    gid_t xgid = gid == 0 ? selfgid : gid;

    if (command[0] != '/') {
        fprintf(stderr, "*** error: command needs to be an absolute path: %s\n", command);
        return 1;
    }

    if (!can_execute(command, xuid, xgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s\n", command);
        return 1;
    }

    if (crash_report != NULL && !can_execute(crash_report, selfuid, selfgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s\n", crash_report);
        return 1;
    }

    int status = 0;
    bool auto_pidfile = false;
    bool auto_logfile = false;
    char *pidfile_runner = NULL;

    if (pidfile == NULL) {
        auto_pidfile = true;

        size_t bufsize = strlen("/var/run/.pid") + strlen(name) + 1;
        char *buf = malloc(bufsize);
        if (buf == NULL) {
            perror("malloc");
            status = 1;
            goto cleanup;
        }

        if (snprintf(buf, bufsize, "/var/run/%s.pid", name) != bufsize) {
            assert(false);
        }
        pidfile = buf;
    } else if (pidfile[0] != '/') {
        fprintf(stderr, "*** error: pidfile needs to be an absolute path: %s\n", pidfile);
        status = 1;
        goto cleanup;
    }

    if (logfile == NULL) {
        auto_logfile = true;

        size_t bufsize = strlen("/var/log/-%Y-%m-%d.log") + strlen(name) + 1;
        char *buf = malloc(bufsize);
        if (buf == NULL) {
            perror("malloc");
            status = 1;
            goto cleanup;
        }

        if (snprintf(buf, bufsize, "/var/log/%s-%%Y-%%m-%%d.log", name) != bufsize) {
            assert(false);
        }
        logfile = buf;
    } else if (logfile[0] != '/') {
        fprintf(stderr, "*** error: logfile needs to be an absolute path: %s\n", logfile);
        status = 1;
        goto cleanup;
    }

    size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
    pidfile_runner = malloc(pidfile_runner_size);
    if (pidfile_runner == NULL) {
        fprintf(stderr, "*** error: malloc: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile) != pidfile_runner_size) {
        assert(false);
    }

    if (!can_read_write(pidfile, selfuid, selfgid)) {
        fprintf(stderr, "*** error: cannot read and write file: %s\n", pidfile);
        status = 1;
        goto cleanup;
    }

    if (!can_read_write(logfile, selfuid, selfgid)) {
        fprintf(stderr, "*** error: cannot read and write file: %s\n", logfile);
        status = 1;
        goto cleanup;
    }

    if (!is_time_format_ok(logfile)) {
        fprintf(stderr, "*** error: illegal logfile name: %s\n", logfile);
        status = 1;
        goto cleanup;
    }

    // TODO: check for existing pidfiles!!

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "*** error: fork failed: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    }

    sigset_t nohup;
    sigemptyset(&nohup);
    sigaddset(&nohup, SIGHUP);
    int result = sigprocmask(SIG_BLOCK, &nohup, NULL);
    assert(result == 0); (void)result;

    signal(SIGTERM, forward_signal);
    signal(SIGINT,  forward_signal);

    if (pid == 0) {
        // child
        if (write_pidfile(pidfile_runner, getpid()) != 0) {
            fprintf(stderr, "*** error: write_pidfile(\"%s\", %u): %s\n", pidfile_runner, getpid(), strerror(errno));
            status = 1;
            goto cleanup;
        }

        for (;;) {
            posix_spawn_file_actions_t file_actions;

            int errnum = posix_spawn_file_actions_init(&file_actions);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_init: %s\n", strerror(errnum));
                status = 1;
                goto cleanup;
            }

            errnum = posix_spawn_file_actions_addclose(&file_actions, STDIN_FILENO);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_addclose(&file_actions, STDIN_FILENO): %s\n", strerror(errnum));
                status = 1;
                goto cleanup;
            }

            int pipefd[2] = { -1, -1 };
            int result = pipe(pipefd);
            if (result != 0) {
                fprintf(stderr, "*** error: pipe(pipefd): %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            errnum = posix_spawn_file_actions_addclose(&file_actions, pipefd[0]);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_addclose(&file_actions, pipefd[0]): %s\n", strerror(errnum));
                status = 1;
                goto cleanup;
            }

            errnum = posix_spawn_file_actions_adddup2(&file_actions, pipefd[1], STDOUT_FILENO);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO, STDERR_FILENO): %s\n", strerror(errnum));
                status = 1;
                goto cleanup;
            }

            errnum = posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO, STDERR_FILENO);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO, STDERR_FILENO): %s\n", strerror(errnum));
                status = 1;
                goto cleanup;
            }

            result = posix_spawnp(&service_pid, command, &file_actions, NULL, command_argv, environ);
            close(pipefd[1]);

            errnum = posix_spawn_file_actions_destroy(&file_actions);
            if (errnum != 0) {
                fprintf(stderr, "*** error: posix_spawn_file_actions_destroy: %s\n", strerror(errnum));
                // but ignore
            }

            if (result != 0) {
                fprintf(stderr, "*** error: spawning %s: %s\n", command, strerror(result));
                close(pipefd[0]);
                status = 1;
                goto cleanup;
            }

            int pidfd = pidfd_open(service_pid, 0);
            if (pidfd == -1) {
                fprintf(stderr, "*** error: pidfd_open(%u): %s\n", service_pid, strerror(errno));
                // XXX: what to do here?
            }

            if (write_pidfile(pidfile, service_pid) != 0) {
                fprintf(stderr, "*** error: write_pidfile(\"%s\", %u): %s\n", pidfile, service_pid, strerror(errno));
                // XXX: what to do here?
            }

            // TODO: poll for pidfd and pipefd[0]

            struct pollfd pollfds[] = {
                { pidfd,     POLLIN, 0 },
                { pipefd[0], POLLIN, 0 },
            };

            size_t open_count = 2;
            while (open_count > 0) {
                result = poll(pollfds, 2, -1);
                if (result < 0) {
                    fprintf(stderr, "*** error: poll(&pollfds, 2, -1): %s\n", strerror(errno));
                    break;
                }

                if (pollfds[0].revents & POLLIN) {
                    // TODO
                }

                if (pollfds[0].revents & POLLHUP) {
                    open_count --; // XXX
                }

                if (pollfds[0].revents & POLLERR) {
                    
                }

                if (pollfds[1].revents & POLLIN) {
                    // TODO
                }

                if (pollfds[1].revents & POLLHUP) {
                    open_count --; // XXX
                }

                if (pollfds[1].revents & POLLERR) {
                    
                }
            }

            close(pipefd[0]);
            close(pidfd);
        }
    }

cleanup:
    free(pidfile_runner);

    if (auto_pidfile) {
        free((char*)pidfile);
    }

    if (auto_logfile) {
        free((char*)logfile);
    }

    return status;
}

const struct option common_options[] = {
    { "pidfile", required_argument, 0, 'p' },
    { 0, 0, 0, 0 },
};

int command_stop(int argc, char *argv[]) {
    (void)common_options;
    return 0;
}

int command_status(int argc, char *argv[]) {
    return 0;
}

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
        usage(argc, argv);
        return 0;
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        usage(argc, argv);
        return 1;
    }

    return 0;
}
