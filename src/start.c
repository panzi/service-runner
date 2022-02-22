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
#include <sys/syscall.h>
#include <sys/wait.h>
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
#include <spawn.h>

#include "service_runner.h"

#ifndef P_PIDFD
    #define P_PIDFD 3
#endif

#define PIPE_READ  0
#define PIPE_WRITE 1
#define SPLICE_SZIE ((size_t)2 * 1024 * 1024 * 1024)

extern char **environ;

enum {
    OPT_START_PIDFILE,
    OPT_START_LOGFILE,
    OPT_START_CHOWN_LOGFILE,
    OPT_START_USER,
    OPT_START_GROUP,
    OPT_START_CRASH_REPORT,
    OPT_START_CRASH_SLEEP,
    OPT_START_COUNT,
};

const struct option start_options[] = {
    [OPT_START_PIDFILE]       = { "pidfile",       required_argument, 0, 'p' },
    [OPT_START_LOGFILE]       = { "logfile",       required_argument, 0, 'l' },
    [OPT_START_CHOWN_LOGFILE] = { "chown-logfile", no_argument,       0,  0  },
    [OPT_START_USER]          = { "user",          required_argument, 0, 'u' },
    [OPT_START_GROUP]         = { "group",         required_argument, 0, 'g' },
    [OPT_START_CRASH_REPORT]  = { "crash-report",  required_argument, 0,  0  },
    [OPT_START_CRASH_SLEEP]   = { "crash-sleep",   required_argument, 0,  0  },
    [OPT_START_COUNT]         = { 0, 0, 0, 0 },
};

pid_t service_pid = 0;
int service_pidfd = -1;
volatile bool running = false;
volatile bool restart = false;
volatile bool got_sigchld = false;

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

    errno = EACCES;
    return false;
}

bool can_read_write(const char *filename, uid_t uid, gid_t gid) {
    struct stat meta;

    if (uid == 0 || gid == 0) {
        // root
        return true;
    }

    if (stat(filename, &meta) != 0) {
        if (errno == ENOENT) {
            char *namedup = strdup(filename);
            if (namedup == NULL) {
                fprintf(stderr, "*** error: strdup(\"%s\"): %s\n", filename, strerror(errno));
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

            errno = EACCES;
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

    if (can_read && can_write) {
        return true;
    }

    errno = EACCES;
    return false;
}

int get_uid_from_name(const char *username, uid_t *uidptr) {
    if (!*username) {
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
        fprintf(stderr, "*** error: malloc(%zu): %s\n", bufsize, strerror(errno));
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
    if (!*groupname) {
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
        fprintf(stderr, "*** error: malloc(%zu): %s\n", bufsize, strerror(errno));
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

void forward_signal(int sig) {
    if (service_pid == 0) {
        fprintf(stderr, "*** error: received signal %d, but service process is not running -> ignored\n", sig);
        return;
    }

    fprintf(stderr, "service-runner: received signal %d, forwarding to service PID %u\n", sig, service_pid);
    running = false;

    if (service_pidfd != -1) {
        if (pidfd_send_signal(service_pidfd, sig, NULL, 0) != 0) {
            if (errno == EBADFD || errno == ENOSYS) {
                fprintf(stderr, "*** error: pidfd_send_signal(service_pidfd, %d, NULL, 0) failed, using kill(%d, %d): %s\n",
                    sig, service_pid, sig, strerror(errno));
                if (service_pid != 0 && kill(service_pid, sig) != 0) {
                    fprintf(stderr, "*** error: forwarding signal %d to PID %d: %s\n", sig, service_pid, strerror(errno));
                }
            } else {
                fprintf(stderr, "*** error: forwarding signal %d to PID %d via pidfd: %s\n", sig, service_pid, strerror(errno));
            }
        }
    } else if (kill(service_pid, sig) != 0) {
        fprintf(stderr, "*** error: forwarding signal %d to PID %d: %s\n", sig, service_pid, strerror(errno));
    }
}

void handle_restart(int sig) {
    if (service_pid == 0) {
        fprintf(stderr, "*** error: received signal %d, but service process is not running -> ignored\n", sig);
        return;
    }

    fprintf(stderr, "service-runner: received signal %d, restarting service...\n", sig);
    restart = true;

    if (service_pidfd != -1) {
        if (pidfd_send_signal(service_pidfd, SIGTERM, NULL, 0) != 0) {
            if (errno == EBADFD || errno == ENOSYS) {
                fprintf(stderr, "*** error: pidfd_send_signal(%d, SIGTERM, NULL, 0) failed, using kill(%d, SIGTERM): %s\n",
                    service_pidfd, service_pid, strerror(errno));
                if (service_pid != 0 && kill(service_pid, SIGTERM) != 0) {
                    fprintf(stderr, "*** error: sending SIGTERM to PID %d: %s\n", service_pid, strerror(errno));
                }
            } else {
                fprintf(stderr, "*** error: sending SIGTERM to PID %d via pidfd: %s\n", service_pid, strerror(errno));
            }
        }
    } else if (kill(service_pid, SIGTERM) != 0) {
        fprintf(stderr, "*** error: sending SIGTERM to PID %d: %s\n", service_pid, strerror(errno));
    }
}

void handle_child(int sig) {
    // for when pidfd is not supported handle child exiting via SIGCHLD and EINTR of poll()
    got_sigchld = true;
}

int command_start(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    int longind = 0;

    const char *pidfile = NULL;
    const char *logfile = NULL;
    const char *user    = NULL;
    const char *group   = NULL;

    bool chown_logfile = false;
    const char *crash_report = NULL;
    unsigned int crash_sleep = 1;

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:l:u:g:", start_options, &longind);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 0:
                switch (longind) {
                    case OPT_START_CHOWN_LOGFILE:
                        chown_logfile = true;
                        break;

                    case OPT_START_CRASH_REPORT:
                        crash_report = optarg;
                        break;

                    case OPT_START_CRASH_SLEEP:
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

    // because of skipped first argument:
    ++ optind;

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

    uid_t selfuid = geteuid();
    uid_t selfgid = getegid();

    uid_t xuid = user  == NULL ? selfuid : uid;
    gid_t xgid = group == NULL ? selfgid : gid;

    if (!can_execute(command, xuid, xgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s\n", command);
        return 1;
    }

    if (crash_report != NULL && !can_execute(crash_report, selfuid, selfgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s: %s\n", crash_report, strerror(errno));
        return 1;
    }

    int status = 0;
    int logfile_fd = -1;

    bool free_pidfile = false;
    bool free_logfile = false;
    bool free_command = false;
    bool cleanup_pidfiles = false;

    char *pidfile_runner = NULL;
    char logfile_path_buf[PATH_MAX];

    if (command[0] != '/') {
        char *buf = abspath(command);
        if (buf == NULL) {
            fprintf(stderr, "*** error: abspath(\"%s\"): %s\n", command, strerror(errno));
            status = 1;
            goto cleanup;
        }

        command = buf;
        free_command = true;
    }

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

    switch (get_logfile_abspath((char**)&logfile, name)) {
        case ABS_PATH_NEW:
            free_logfile = true;
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

    if (!can_read_write(pidfile, selfuid, selfgid)) {
        fprintf(stderr, "*** error: cannot read and write file: %s: %s\n", pidfile, strerror(errno));
        status = 1;
        goto cleanup;
    }

    {
        // checking for existing process and trying to deal with it
        pid_t runner_pid = 0;
        if (read_pidfile(pidfile_runner, &runner_pid) == 0) {
            if (kill(runner_pid, 0) == 0) {
                printf("%s is already running\n", name);
                goto cleanup;
            } else {
                fprintf(stderr, "*** error: %s exists, but PID %d doesn't exist.\n", pidfile_runner, runner_pid);
                pid_t other_pid = 0;
                if (read_pidfile(pidfile, &other_pid) == 0) {
                    if (kill(other_pid, 0) == 0) {
                        fprintf(stderr, "*** error: Service is running (PID: %d), but it's service-runner is not!\n", other_pid);
                        fprintf(stderr, "           You probably will want to kill that process?\n");
                        status = 1;
                        goto cleanup;
                    }

                    if (unlink(pidfile) != 0 && errno != ENOENT) {
                        fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile, strerror(errno));
                    }
                }

                if (unlink(pidfile_runner) != 0 && errno != ENOENT) {
                    fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile_runner, strerror(errno));
                }
            }
        }
    }

    const bool do_logrotate = strchr(logfile, '%') != NULL;
    const char *logfile_path;

    if (do_logrotate) {
        time_t now = time(NULL);
        struct tm local_now;
        if (localtime_r(&now, &local_now) == NULL) {
            fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (strftime(logfile_path_buf, sizeof(logfile_path_buf), logfile, &local_now) == 0) {
            fprintf(stderr, "*** error: cannot format logfile \"%s\": %s\n", logfile, strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (!can_read_write(logfile_path_buf, selfuid, selfgid)) {
            fprintf(stderr, "*** error: cannot read and write file: %s\n", logfile);
            status = 1;
            goto cleanup;
        }

        logfile_path = logfile_path_buf;
    } else {
        logfile_path = logfile;
    }

    logfile_fd = open(logfile_path, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
    if (logfile_fd == -1) {
        fprintf(stderr, "*** error: cannot open logfile: %s: %s\n", logfile_path, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (chown_logfile && fchown(logfile_fd, xuid, xgid) != 0) {
        fprintf(stderr, "*** error: cannot change owner of logfile: %s: %s\n", logfile_path, strerror(errno));
        status = 1;
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "*** error: fork for deamonize failed: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    } else if (pid != 0) {
        // parent
        goto cleanup;
    }

    // child
    {
        // block signals until forked service process
        // Can't install the signal handlers in here, because they would
        // also run in the child, but if I don't block these signals
        // service-runner might terminate in an invalid state concerning
        // created pidfiles and such.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGHUP);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGCHLD);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
            fprintf(stderr, "*** error: sigprocmask(SIG_BLOCK, &mask, NULL): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    {
        // setup standard I/O
        if (close(STDIN_FILENO) != 0 && errno != EBADFD) {
            fprintf(stderr, "*** error: close(STDIN_FILENO): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        int stdin_fd = open("/dev/null", O_RDONLY);
        if (stdin_fd == -1) {
            fprintf(stderr, "*** error: open(\"/dev/null\", O_RDONLY): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (stdin_fd != STDIN_FILENO) {
            if (dup2(stdin_fd, STDIN_FILENO) == -1) {
                fprintf(stderr, "*** error: dup2(stdin_fd, STDIN_FILENO): %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (close(stdin_fd) != 0) {
                fprintf(stderr, "*** error: close(stdin_fd): %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }
        }

        if (dup2(logfile_fd, STDOUT_FILENO) == -1) {
            fprintf(stderr, "*** error: dup2(logfile_fd, STDOUT_FILENO): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (dup2(logfile_fd, STDERR_FILENO) == -1) {
            fprintf(stderr, "*** error: dup2(logfile_fd, STDERR_FILENO): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (write_pidfile(pidfile_runner, getpid()) != 0) {
            fprintf(stderr, "*** error: write_pidfile(\"%s\", %u): %s\n", pidfile_runner, getpid(), strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    cleanup_pidfiles = true;
    running = true;
    while (running) {
        int pipefd[2] = { -1, -1 };

        if (do_logrotate) {
            // logging pipe
            // if no log-rotating is done stdout/stderr pipes directly to the logfile, no need for the pipe
            int result = pipe(pipefd);
            if (result != 0) {
                fprintf(stderr, "*** error: pipe(pipefd): %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            int flags = fcntl(pipefd[PIPE_READ], F_GETFL, 0);
            if (flags == -1) {
                fprintf(stderr, "*** error: fcntl(pipefd[PIPE_READ], F_GETFL, 0): %s\n", strerror(errno));
                flags = 0;
            }

            if (fcntl(pipefd[PIPE_READ], F_SETFL, flags | O_NONBLOCK) == -1) {
                fprintf(stderr, "*** error: fcntl(pipefd[PIPE_READ], F_SETFL, flags | O_NONBLOCK): %s\n", strerror(errno));
            }
        }

        service_pid = fork();

        if (service_pid < 0) {
            fprintf(stderr, "*** error: fork for starting service failed: %s\n", strerror(errno));
            status = 1;
            if (do_logrotate) {
                close(pipefd[PIPE_READ]);
                close(pipefd[PIPE_WRITE]);
            }
            goto cleanup;
        } else if (service_pid == 0) {
            // child
            if (write_pidfile(pidfile, getpid()) != 0) {
                fprintf(stderr, "*** error: write_pidfile(\"%s\", %u): %s\n", pidfile, getpid(), strerror(errno));
                status = 1;
                if (do_logrotate) {
                    close(pipefd[PIPE_READ]);
                    close(pipefd[PIPE_WRITE]);
                }
                goto cleanup;
            }

            if (group != NULL && setgroups(1, (gid_t[]){ gid }) != 0) {
                fprintf(stderr, "*** error: (child) setgroups(1, (gid_t[]){ %u }): %s\n", gid, strerror(errno));
                status = 1;
                if (do_logrotate) {
                    close(pipefd[PIPE_READ]);
                    close(pipefd[PIPE_WRITE]);
                }
                goto cleanup;
            }

            if (user != NULL && setuid(uid) != 0) {
                fprintf(stderr, "*** error: (child) setuid(%u): %s\n", uid, strerror(errno));
                status = 1;
                if (do_logrotate) {
                    close(pipefd[PIPE_READ]);
                    close(pipefd[PIPE_WRITE]);
                }
                goto cleanup;
            }

            if (do_logrotate) {
                if (close(pipefd[PIPE_READ]) != 0) {
                    fprintf(stderr, "*** error: (child) close(pipefd[PIPE_READ]): %s\n", strerror(errno));
                    // though, ignore it anyway?
                }

                if (dup2(pipefd[PIPE_WRITE], STDOUT_FILENO) == -1) {
                    fprintf(stderr, "*** error: (child) dup2(pipefd[PIPE_WRITE], STDOUT_FILENO): %s\n", strerror(errno));
                    status = 1;
                    close(pipefd[PIPE_WRITE]);
                    goto cleanup;
                }

                if (dup2(pipefd[PIPE_WRITE], STDERR_FILENO) == -1) {
                    fprintf(stderr, "*** error: (child) dup2(pipefd[PIPE_WRITE], STDERR_FILENO): %s\n", strerror(errno));
                    status = 1;
                    close(pipefd[PIPE_WRITE]);
                    goto cleanup;
                }

                if (close(pipefd[PIPE_WRITE]) != 0) {
                    fprintf(stderr, "*** error: (child) close(pipefd[PIPE_WRITE]): %s\n", strerror(errno));
                    // though, ignore it anyway?
                }
            }

            // signal masks are preserved across exec*()
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGTERM);
            sigaddset(&mask, SIGINT);
            sigaddset(&mask, SIGUSR1);
            sigaddset(&mask, SIGCHLD);
            if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                fprintf(stderr, "*** error: (child) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s\n", strerror(errno));
                status = 1;
                goto cleanup;
            }

            execv(command, command_argv);
            fprintf(stderr, "*** error: (child) execv(\"%s\", command_argv): %s\n", command, strerror(errno));
            status = 1;
            goto cleanup;
        } else {
            // parent
            {
                // setup signal handling
                if (signal(SIGTERM, forward_signal) == SIG_ERR) {
                    fprintf(stderr, "*** error: (parent) signal(SIGTERM, forward_signal): %s\n", strerror(errno));
                }

                if (signal(SIGINT, forward_signal) == SIG_ERR) {
                    fprintf(stderr, "*** error: (parent) signal(SIGINT, forward_signal): %s\n", strerror(errno));
                }

                if (signal(SIGUSR1, handle_restart) == SIG_ERR) {
                    fprintf(stderr, "*** error: (parent) signal(SIGUSR1, handle_restart): %s\n", strerror(errno));
                }

                sigset_t mask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGTERM);
                sigaddset(&mask, SIGINT);
                sigaddset(&mask, SIGUSR1);
                if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                    fprintf(stderr, "*** error: (parent) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s\n", strerror(errno));
                }
            }

            if (do_logrotate && close(pipefd[PIPE_WRITE]) != 0) {
                fprintf(stderr, "*** error: (parent) close(pipefd[PIPE_WRITE]): %s\n", strerror(errno));
                // though, ignore it anyway?
            }

            service_pidfd = pidfd_open(service_pid, 0);
            if (service_pidfd == -1 && errno != ENOSYS) {
                fprintf(stderr, "*** error: (parent) pidfd_open(%u): %s\n", service_pid, strerror(errno));

                // wait a bit so the signal handlers are resetted again in child
                struct timespec wait_time = {
                    .tv_sec  = 0,
                    .tv_nsec = 500000000,
                };
                nanosleep(&wait_time, NULL);

                if (kill(service_pid, SIGTERM) != 0 && errno != ESRCH) {
                    fprintf(stderr, "*** error: (parent) kill(%u, SIGTERM): %s\n", service_pid, strerror(errno));
                }

                for (;;) {
                    int child_status = 0;
                    int result = waitpid(service_pid, &child_status, 0);
                    if (result < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno != ECHILD) {
                            fprintf(stderr, "*** error: (parent) waitpid(%u, &status, 0): %s\n", service_pid, strerror(errno));
                        }
                    } else if (result == 0) {
                        assert(false);
                        continue;
                    } else if (child_status != 0) {
                        if (WIFSIGNALED(child_status)) {
                            fprintf(stderr, "*** error: service PID %u exited with signal %d\n", service_pid, WTERMSIG(child_status));
                        } else {
                            fprintf(stderr, "*** error: service PID %u exited with status %d\n", service_pid, WEXITSTATUS(child_status));
                        }
                    }
                    break;
                }

                status = 1;
                goto cleanup;
            }

            #define POLLFD_PID  0
            #define POLLFD_PIPE 1

            struct pollfd pollfds[] = {
                [POLLFD_PID ] = { service_pidfd,     POLLIN, 0 },
                [POLLFD_PIPE] = { pipefd[PIPE_READ], do_logrotate ? POLLIN : 0, 0 },
            };

            if (service_pidfd == -1) {
                // for systems that don't support pidfd
                pollfds[POLLFD_PID ].revents = 0;
                if (signal(SIGCHLD, handle_child) != 0) {
                    fprintf(stderr, "*** error: signal(SIGCHLD, handle_child): %s\n", strerror(errno));
                }
            }

            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                fprintf(stderr, "*** error: (parent) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s\n", strerror(errno));
            }

            while (pollfds[POLLFD_PID].events != 0 || pollfds[POLLFD_PIPE].events != 0) {
                pollfds[POLLFD_PID ].revents = 0;
                pollfds[POLLFD_PIPE].revents = 0;

                int result =
                    pollfds[0].events == 0 ? poll(pollfds + 1, 1, -1) :
                    pollfds[1].events == 0 ? poll(pollfds,     1, -1) :
                                             poll(pollfds,     2, -1);
                if (result < 0) {
                    if (errno != EINTR) {
                        fprintf(stderr, "*** error: (parent) poll(): %s\n", strerror(errno));
                        break;
                    }
                }

                if (do_logrotate) {
                    if (pollfds[POLLFD_PIPE].revents & POLLIN) {
                        // log-handling
                        time_t now = time(NULL);
                        struct tm local_now;
                        char new_logfile_path_buf[PATH_MAX];

                        if (localtime_r(&now, &local_now) == NULL) {
                            fprintf(stderr, "*** error: (parent) getting local time: %s\n", strerror(errno));
                            status = 1;
                            goto cleanup;
                        }

                        if (strftime(new_logfile_path_buf, sizeof(new_logfile_path_buf), logfile, &local_now) == 0) {
                            fprintf(stderr, "*** error: (parent) cannot format logfile \"%s\": %s\n", logfile, strerror(errno));
                            status = 1;
                            goto cleanup;
                        }

                        if (strcmp(new_logfile_path_buf, logfile_path_buf) != 0) {
                            int new_logfile_fd = open(new_logfile_path_buf, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
                            if (logfile_fd == -1) {
                                fprintf(stderr, "*** error: (parent) cannot open logfile: %s: %s\n", new_logfile_path_buf, strerror(errno));
                            }

                            if (chown_logfile && fchown(new_logfile_fd, xuid, xgid) != 0) {
                                fprintf(stderr, "*** error: (parent) cannot change owner of logfile: %s: %s\n", new_logfile_path_buf, strerror(errno));
                            }

                            if (close(logfile_fd) != 0) {
                                fprintf(stderr, "*** error: (parent) close(logfile_fd): %s\n", strerror(errno));
                            }

                            logfile_fd = new_logfile_fd;
                            if (dup2(logfile_fd, STDOUT_FILENO) == -1) {
                                fprintf(stderr, "*** error: (parent) dup2(logfile_fd, STDOUT_FILENO): %s\n", strerror(errno));
                            }

                            if (dup2(logfile_fd, STDERR_FILENO) == -1) {
                                fprintf(stderr, "*** error: (parent) dup2(logfile_fd, STDERR_FILENO): %s\n", strerror(errno));
                            }

                            strcpy(logfile_path_buf, new_logfile_path_buf);
                        }

                        // handle log messages
                        ssize_t count = splice(pipefd[PIPE_READ], NULL, logfile_fd, NULL, SPLICE_SZIE, SPLICE_F_NONBLOCK);
                        if (count < 0) {
                            fprintf(stderr, "*** error: (parent) splice(%d /* pipefd[PIPE_READ] */, NULL, %d /* logfile_fd */, NULL, %zu /* SPLICE_SZIE */, SPLICE_F_NONBLOCK): %s\n",
                                pipefd[PIPE_READ], logfile_fd, SPLICE_SZIE, strerror(errno));
                        }
                    }

                    if (pollfds[POLLFD_PIPE].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        pollfds[POLLFD_PIPE].events = 0;
                    }
                }

                if ((pollfds[POLLFD_PID].revents & POLLIN) || got_sigchld) {
                    got_sigchld = false;
                    // waitid() doesn't work for some reason! always produces ECHLD
                    int service_status = 0;
                    pid_t result = waitpid(service_pid, &service_status, WNOHANG);
                    if (result == 0) {
                        // would have blocked
                    } else if (result == -1) {
                        pollfds[POLLFD_PID].events = 0;
                        fprintf(stderr, "*** error: (parent) waitpid(%d, &service_status, WNOHANG): %s\n", service_pid, strerror(errno));
                    } else {
                        // block signals again until we're ready again
                        sigset_t mask;
                        sigemptyset(&mask);
                        sigaddset(&mask, SIGTERM);
                        sigaddset(&mask, SIGINT);
                        sigaddset(&mask, SIGUSR1);
                        sigaddset(&mask, SIGCHLD);
                        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
                            fprintf(stderr, "*** error: (parent) sigprocmask(SIG_BLOCK, &mask, NULL): %s\n", strerror(errno));
                        }

                        pollfds[POLLFD_PID].events = 0;
                        bool crash = false;
                        int param = 0;
                        const char *code_str = NULL;

                        if (WIFEXITED(service_status)) {
                            param = WEXITSTATUS(service_status);
                            code_str = "EXITED";

                            if (param == 0) {
                                printf("service-runner: %s exited normally\n", name);
                                if (restart) {
                                    // don't set running to false
                                    restart = false;
                                } else {
                                    running = false;
                                }
                            } else {
                                fprintf(stderr, "service-runner: *** error: %s exited with error status %d\n", name, param);
                                crash = true;
                            }
                        } else if (WIFSIGNALED(service_status)) {
                            param = WTERMSIG(service_status);
                            if (WCOREDUMP(service_status)) {
                                code_str = "DUMPED";
                                fprintf(stderr, "service-runner: *** error: %s was killed by signal %d and dumped core\n", name, param);
                                crash = true;
                            } else {
                                code_str = "KILLED";
                                fprintf(stderr, "service-runner: *** error: %s was killed by signal %d\n", name, param);

                                switch (param) {
                                    case SIGTERM:
                                        if (restart) {
                                            // don't set running to false
                                            restart = false;
                                            break;
                                        }
                                    case SIGINT:
                                    case SIGKILL:
                                        printf("service-runner: service stopped via signal %d -> don't restart\n", param);
                                        running = false;
                                        break;
                                }
                            }
                        } else {
                            assert(false);
                        }

                        service_pid = -1;

                        if (crash) {
                            struct timespec ts_before;
                            struct timespec ts_after;
                            bool time_ok = true;

                            if (crash_report == NULL) {
                                time_ok = false;
                            } else {
                                if (clock_gettime(CLOCK_MONOTONIC, &ts_before) != 0) {
                                    time_ok = false;
                                    fprintf(stderr, "*** error: (parent) clock_gettime(CLOCK_MONOTONIC, &ts_before): %s\n", strerror(errno));
                                }

                                char param_str[24];
                                int result = snprintf(param_str, sizeof(param_str), "%d", param);
                                assert(result > 0 && result < sizeof(param_str));

                                pid_t report_pid = 0;
                                result = posix_spawn(&report_pid, crash_report, NULL, NULL,
                                    (char*[]){ (char*)crash_report, (char*)name, (char*)code_str, param_str, (char*)logfile_path, NULL },
                                    environ);

                                if (result != 0) {
                                    fprintf(stderr, "*** error: (parent) starting crash reporter: %s\n", strerror(errno));
                                } else {
                                    for (;;) {
                                        int report_status = 0;
                                        int result = waitpid(report_pid, &report_status, 0);
                                        if (result < 0) {
                                            if (errno == EINTR) {
                                                continue;
                                            }
                                            fprintf(stderr, "*** error: (parent) waitpid(%u, &report_status, 0): %s\n", report_pid, strerror(errno));
                                        } else if (result == 0) {
                                            assert(false);
                                            continue;
                                        } else if (report_status != 0) {
                                            if (WIFSIGNALED(report_status)) {
                                                fprintf(stderr, "*** error: crash report PID %u exited with signal %d\n", report_pid, WTERMSIG(report_status));
                                            } else {
                                                fprintf(stderr, "*** error: crash report PID %u exited with status %d\n", report_pid, WEXITSTATUS(report_status));
                                            }
                                        }
                                        break;
                                    }
                                }

                                if (time_ok && clock_gettime(CLOCK_MONOTONIC, &ts_after) != 0) {
                                    time_ok = false;
                                    fprintf(stderr, "*** error: (parent) clock_gettime(CLOCK_MONOTONIC, &ts_after): %s\n", strerror(errno));
                                }
                            }

                            if (running && crash_sleep) {
                                if (time_ok) {
                                    time_t secs = ts_after.tv_sec - ts_before.tv_sec;
                                    if (secs <= crash_sleep) {
                                        sleep(crash_sleep - secs);
                                    }
                                } else {
                                    sleep(crash_sleep);
                                }
                            }
                        }
                    }
                }

                if (pollfds[POLLFD_PID].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    pollfds[POLLFD_PID].events = 0;
                }
            }

            if (do_logrotate && close(pipefd[PIPE_READ]) != 0) {
                fprintf(stderr, "*** error: (parent) close(pipefd[PIPE_READ]): %s\n", strerror(errno));
            }

            if (service_pidfd != -1 && close(service_pidfd) != 0) {
                fprintf(stderr, "*** error: (parent) close(pidfd): %s\n", strerror(errno));
            }
            service_pidfd = -1;
        }
    }

cleanup:
    if (cleanup_pidfiles) {
        if (unlink(pidfile) != 0 && errno != ENOENT) {
            fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile, strerror(errno));
        }

        if (unlink(pidfile_runner) != 0 && errno != ENOENT) {
            fprintf(stderr, "*** error: unlink(\"%s\"): %s\n", pidfile_runner, strerror(errno));
        }
    }

    free(pidfile_runner);

    if (free_pidfile) {
        free((char*)pidfile);
    }

    if (free_logfile) {
        free((char*)logfile);
    }

    if (free_command) {
        free((char*)command);
    }

    if (logfile_fd != -1) {
        close(logfile_fd);
    }

    return status;
}
