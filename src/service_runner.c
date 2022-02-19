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

#define pidfd_open(pid, flags) \
    syscall(SYS_pidfd_open, (pid), (flags))

#define pidfd_send_signal(pidfd, sig, info, flags) \
    syscall(SYS_pidfd_send_signal, (pidfd), (sig), (info), (flags))

#ifndef P_PIDFD
    #define P_PIDFD 3
#endif

#define PIPE_READ  0
#define PIPE_WRITE 1
#define SPLICE_SZIE ((size_t)2 * 1024 * 1024 * 1024)

extern char **environ;

#define HELP_OPT_PIDFILE \
        "       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: /var/run/NAME.pid\n"

#define HELP_CMD_START                                                                                                          \
        "   %s start <name> [options] [--] <command> [argument...]\n"                                                           \
        "\n"                                                                                                                    \
        "       Start <command> as service <name>. Does nothing if the service is already running.\n"                           \
        "       This automatically deamonizes, handles PID- and log-files, and restarts on crash.\n"                            \
        "\n"                                                                                                                    \
        "   OPTIONS:\n"                                                                                                         \
        HELP_OPT_PIDFILE                                                                                                        \
        "                                       Note that a second pidfile with the name FILE.runner is created containing\n"   \
        "                                       the process ID of the service-runner process itself.\n"                         \
        "       --logfile=FILE, -l FILE         Write service output to FILE. default: /var/log/NAME-%%Y-%%m-%%d.log\n"         \
        "                                       This implements log-rotating based on the file name pattern.\n"                 \
        "                                       See `man strftime` for a description of the pattern language.\n"                \
        "       --user=USER, -u USER            Run service as USER (name or UID).\n"                                           \
        "       --group=GROUP, -g GROUP         Run service as GROUP (name or GID).\n"                                          \
        "       --crash-sleep=SECONDS           Wait SECONDS before restarting service. default: 1\n"                           \
        "       --crash-report=COMMAND          Run `COMMAND CODE STATUS LOGFILE` if the service crashed.\n"                    \
        "                                       CODE values:\n"                                                                 \
        "                                         EXITED ... service has exited, STATUS is it's exit status\n"                  \
        "                                         KILLED ... service was killed, STATUS is the killing signal\n"                \
        "                                         DUMPED ... service produced a core dump, STATUS is the killing signal\n"

#define HELP_CMD_STOP                                                                                                                   \
        "   %s stop <name> [options]\n"                                                                                                 \
        "\n"                                                                                                                            \
        "       Stop service <name>. If --pidfile was passed to the corresponding start command it must be passed with\n"               \
        "       the same argument here again.\n"                                                                                        \
        "\n"                                                                                                                            \
        "   OPTIONS:\n"                                                                                                                 \
        HELP_OPT_PIDFILE                                                                                                                \
        "       --shutdown-timeout=SECONDS      If the service doesn't shut down after SECONDS after sending SIGTERM send SIGKILL.\n"   \
        "                                       0 means no timeout, just wait forever. default: 0\n"

#define HELP_CMD_STATUS                                                 \
        "   %s status <name> [options]\n"                               \
        "\n"                                                            \
        "       Print some status information about service <name>.\n"  \
        "\n"                                                            \
        "   OPTIONS:\n"                                                 \
        HELP_OPT_PIDFILE

#define HELP_CMD_HELP               \
        "   %s help [command]\n"    \
        "\n"                        \
        "       Print help message to <command>. If no command is passed, prints help message to all commands.\n"

const char *get_progname(int argc, char *argv[]) {
    return argc > 0 ? argv[0] : "service_runner";
}

void short_usage(int argc, char *argv[]) {
    const char *progname = get_progname(argc, argv);
    printf("\n");
    printf("Usage: %s start  <name> [options] [--] <command> [argument...]\n", progname);
    printf("       %s stop   <name> [options]\n", progname);
    printf("       %s status <name> [options]\n", progname);
    printf("       %s help [command]\n", progname);
}

void usage(int argc, char *argv[]) {
    short_usage(argc, argv);
    const char *progname = get_progname(argc, argv);
    printf(
        "\n"
        "COMMANDS:\n"
        "\n"
    );
    printf(HELP_CMD_START  "\n", progname);
    printf(HELP_CMD_STOP   "\n", progname);
    printf(HELP_CMD_STATUS "\n", progname);
    printf(HELP_CMD_HELP   "\n", progname);
    printf(
        "(c) 2022 Mathias Panzenböck\n"
        "GitHub: https://github.com/panzi/service-runner\n"
    );
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
    OPT_START_PIDFILE,
    OPT_START_LOGFILE,
    OPT_START_USER,
    OPT_START_GROUP,
    OPT_START_CRASH_REPORT,
    OPT_START_CRASH_SLEEP,
    OPT_START_COUNT,
};

enum {
    OPT_STOP_PIDFILE,
    OPT_STOP_SHUTDOWN_TIMEOUT,
    OPT_STOP_COUNT,
};

enum {
    OPT_STATUS_PIDFILE,
    OPT_STATUS_COUNT,
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

size_t dirend(const char *path) {
    size_t index = strlen(path);
    if (index == 0) {
        return 0;
    }

    // select last character
    -- index;

    // skip trailing '/'
    while (index > 0 && path[index] == '/') {
        -- index;
    }

    // skip filename
    while (index > 0 && path[index] != '/') {
        -- index;
    }

    // skip doubled '/'
    while (index > 0 && path[index - 1] == '/') {
        -- index;
    }

    return index;
}

char *abspath(const char *path) {
    char *tmp = realpath(path, NULL);
    if (tmp != NULL || errno != ENOENT) {
        return tmp;
    }

    tmp = strdup(path);
    if (tmp == NULL) {
        return NULL;
    }

    size_t path_len = strlen(tmp);
    size_t prev_index = path_len;
    char prev_char = 0;
    for (;;) {
        size_t index = dirend(tmp);
        tmp[prev_index] = prev_char;

        if (index == 0) {
            char curdir[PATH_MAX];
            if (getcwd(curdir, sizeof(curdir)) == NULL) {
                free(tmp);
                return NULL;
            }

            size_t bufsize = strlen(curdir) + path_len + 2;
            char *buf = malloc(bufsize);
            if (buf == NULL) {
                free(tmp);
                return NULL;
            }
            int count = snprintf(buf, bufsize, "%s/%s", curdir, path);
            assert(count >= 0 && (size_t)count == bufsize - 1);
            free(tmp);
            return buf;
        }

        prev_char = tmp[index];
        tmp[index] = 0;

        char *parent = realpath(tmp, NULL);
        if (parent != NULL) {
            while (path[index] == '/') {
                ++ index;
            }
            size_t bufsize = strlen(parent) + path_len - index + 2;

            char *buf = malloc(bufsize);
            if (buf == NULL) {
                free(parent);
                free(tmp);
                return NULL;
            }
            int count = snprintf(buf, bufsize, "%s/%s", parent, path + index);
            assert(count >= 0 && (size_t)count == bufsize - 1);
            free(parent);
            free(tmp);
            return buf;
        } else if (errno == ENOENT) {
            // keep going up
            prev_index = index;
        } else {
            free(tmp);
            return NULL;
        }
    }
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

const struct option start_options[] = {
    [OPT_START_PIDFILE]      = { "pidfile",      required_argument, 0, 'p' },
    [OPT_START_LOGFILE]      = { "logfile",      required_argument, 0, 'l' },
    [OPT_START_USER]         = { "user",         required_argument, 0, 'u' },
    [OPT_START_GROUP]        = { "group",        required_argument, 0, 'g' },
    [OPT_START_CRASH_REPORT] = { "crash-report", required_argument, 0,  0  },
    [OPT_START_CRASH_SLEEP]  = { "crash-sleep",  required_argument, 0,  0  },
    [OPT_START_COUNT]        = { 0, 0, 0, 0 },
};

pid_t service_pid = 0;
int service_pidfd = -1;
volatile bool running = false;

void forward_signal(int sig) {
    running = false;

    if (service_pidfd != -1 && pidfd_send_signal(service_pidfd, sig, NULL, 0) != 0) {
        if (errno == EBADFD || errno == ENOSYS) {
            if (service_pid != 0 && kill(service_pid, sig) != 0) {
                fprintf(stderr, "*** error: forwarding signal %d to PID %d: %s\n", sig, service_pid, strerror(errno));
            }
        } else {
            fprintf(stderr, "*** error: forwarding signal %d to PID %d via pidfd: %s\n", sig, service_pid, strerror(errno));
        }
    }
}

enum AbsPathResult {
    ABS_PATH_ERR,
    ABS_PATH_ORIG,
    ABS_PATH_NEW,
};

enum AbsPathResult get_pidfile_abspath(char **pidfile_ptr, const char *name) {
    char *pidfile = *pidfile_ptr;
    if (pidfile == NULL) {
        size_t bufsize = strlen("/var/run/.pid") + strlen(name) + 1;
        char *buf = malloc(bufsize);
        if (buf == NULL) {
            fprintf(stderr, "*** error: malloc(%zu): %s\n", bufsize, strerror(errno));
            return ABS_PATH_ERR;
        }

        if (snprintf(buf, bufsize, "/var/run/%s.pid", name) != bufsize - 1) {
            assert(false);
        }

        *pidfile_ptr = buf;
        return ABS_PATH_NEW;
    } else if (pidfile[0] != '/') {
        char *buf = abspath(pidfile);
        if (buf == NULL) {
            fprintf(stderr, "*** error: abspath(\"%s\"): %s\n", pidfile, strerror(errno));
            return ABS_PATH_ERR;
        }

        *pidfile_ptr = buf;
        return ABS_PATH_NEW;
    } else {
        return ABS_PATH_ORIG;
    }
}

enum AbsPathResult get_logfile_abspath(char **logfile_ptr, const char *name) {
    char *logfile = *logfile_ptr;
    if (logfile == NULL) {
        size_t bufsize = strlen("/var/log/-%Y-%m-%d.log") + strlen(name) + 1;
        char *buf = malloc(bufsize);
        if (buf == NULL) {
            fprintf(stderr, "*** error: malloc(%zu): %s\n", bufsize, strerror(errno));
            return ABS_PATH_ERR;
        }

        if (snprintf(buf, bufsize, "/var/log/%s-%%Y-%%m-%%d.log", name) != bufsize - 1) {
            assert(false);
        }

        *logfile_ptr = buf;
        return ABS_PATH_NEW;
    } else if (logfile[0] != '/') {
        char *buf = abspath(logfile);
        if (buf == NULL) {
            fprintf(stderr, "*** error: abspath(\"%s\"): %s\n", logfile, strerror(errno));
            return ABS_PATH_ERR;
        }

        *logfile_ptr = buf;
        return ABS_PATH_NEW;
    } else {
        return ABS_PATH_ORIG;
    }

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

    uid_t selfuid = getuid();
    uid_t selfgid = getgid();

    uid_t xuid = uid == 0 ? selfuid : uid;
    gid_t xgid = gid == 0 ? selfgid : gid;

    if (!can_execute(command, xuid, xgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s\n", command);
        return 1;
    }

    if (crash_report != NULL && !can_execute(crash_report, selfuid, selfgid)) {
        fprintf(stderr, "*** error: file not found or not executable: %s\n", crash_report);
        return 1;
    }

    int status = 0;
    int logfile_fd = -1;

    bool free_pidfile = false;
    bool free_logfile = false;
    bool free_command = false;
    bool cleanup_pidfiles = false;

    char *pidfile_runner = NULL;
    char logfile_path[PATH_MAX];
    char new_logfile_path[PATH_MAX];

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

    size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
    pidfile_runner = malloc(pidfile_runner_size);
    if (pidfile_runner == NULL) {
        fprintf(stderr, "*** error: malloc: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile) != pidfile_runner_size - 1) {
        assert(false);
    }

    if (!can_read_write(pidfile, selfuid, selfgid)) {
        fprintf(stderr, "*** error: cannot read and write file: %s\n", pidfile);
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
                    if (kill(other_pid, 0) != 0) {
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

    const bool logfile_has_format = strchr(logfile, '%') != NULL;

    if (logfile_has_format) {
        time_t now = time(NULL);
        struct tm local_now;
        if (localtime_r(&now, &local_now) == NULL) {
            fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (strftime(logfile_path, sizeof(logfile_path), logfile, &local_now) == 0) {
            fprintf(stderr, "*** error: cannot format logfile \"%s\": %s\n", logfile, strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (!can_read_write(logfile_path, selfuid, selfgid)) {
            fprintf(stderr, "*** error: cannot read and write file: %s\n", logfile);
            status = 1;
            goto cleanup;
        }

        logfile_fd = open(logfile_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (logfile_fd == -1) {
            fprintf(stderr, "*** error: cannot open logfile: %s: %s\n", logfile_path, strerror(errno));
            status = 1;
            goto cleanup;
        }
    } else {
        logfile_fd = open(logfile, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (logfile_fd == -1) {
            fprintf(stderr, "*** error: cannot open logfile: %s: %s\n", logfile, strerror(errno));
            status = 1;
            goto cleanup;
        }
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
        // setup signal handling
        sigset_t nohup;
        sigemptyset(&nohup);
        sigaddset(&nohup, SIGHUP);
        int result = sigprocmask(SIG_BLOCK, &nohup, NULL);
        assert(result == 0); (void)result;

        signal(SIGTERM, forward_signal);
        signal(SIGINT,  forward_signal);
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
            if (dup2(stdin_fd, STDIN_FILENO) != 0) {
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

        if (dup2(logfile_fd, STDOUT_FILENO) != 0) {
            fprintf(stderr, "*** error: dup2(logfile_fd, STDOUT_FILENO): %s\n", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (dup2(logfile_fd, STDERR_FILENO) != 0) {
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

        {
            // logging pipe
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
            close(pipefd[PIPE_READ]);
            close(pipefd[PIPE_WRITE]);
            goto cleanup;
        } else if (service_pid == 0) {
            // child
            if (write_pidfile(pidfile, getpid()) != 0) {
                fprintf(stderr, "*** error: write_pidfile(\"%s\", %u): %s\n", pidfile, getpid(), strerror(errno));
                status = 1;
                close(pipefd[PIPE_READ]);
                close(pipefd[PIPE_WRITE]);
                goto cleanup;
            }

            if (gid != 0 && setgroups(1, (gid_t[]){ gid }) != 0) {
                fprintf(stderr, "*** error: child setgroups(1, (gid_t[]){ %u }): %s\n", gid, strerror(errno));
                status = 1;
                close(pipefd[PIPE_READ]);
                close(pipefd[PIPE_WRITE]);
                goto cleanup;
            }

            if (uid != 0 && setuid(uid) != 0) {
                fprintf(stderr, "*** error: child setuid(%u): %s\n", uid, strerror(errno));
                status = 1;
                close(pipefd[PIPE_READ]);
                close(pipefd[PIPE_WRITE]);
                goto cleanup;
            }

            if (close(pipefd[PIPE_READ]) != 0) {
                fprintf(stderr, "*** error: child close(pipefd[PIPE_READ]): %s\n", strerror(errno));
                // though, ignore it anyway?
            }

            if (dup2(pipefd[PIPE_WRITE], STDOUT_FILENO) != 0) {
                fprintf(stderr, "*** error: child dup2(pipefd[PIPE_WRITE], STDOUT_FILENO): %s\n", strerror(errno));
                status = 1;
                close(pipefd[PIPE_WRITE]);
                goto cleanup;
            }

            if (dup2(pipefd[PIPE_WRITE], STDERR_FILENO) != 0) {
                fprintf(stderr, "*** error: child dup2(pipefd[PIPE_WRITE], STDERR_FILENO): %s\n", strerror(errno));
                status = 1;
                close(pipefd[PIPE_WRITE]);
                goto cleanup;
            }

            if (close(pipefd[PIPE_WRITE]) != 0) {
                fprintf(stderr, "*** error: child close(pipefd[PIPE_WRITE]): %s\n", strerror(errno));
                // though, ignore it anyway?
            }

            execv(command, command_argv);
            fprintf(stderr, "*** error: child execv(\"%s\", command_argv): %s\n", command, strerror(errno));
            status = 1;
            goto cleanup;
        } else {
            // parent
            if (close(pipefd[PIPE_WRITE]) != 0) {
                fprintf(stderr, "*** error: parent close(pipefd[PIPE_WRITE]): %s\n", strerror(errno));
                // though, ignore it anyway?
            }

            service_pidfd = pidfd_open(service_pid, 0);
            if (service_pidfd == -1) {
                fprintf(stderr, "*** error: parent pidfd_open(%u): %s\n", service_pid, strerror(errno));

                if (kill(service_pid, SIGTERM) != 0 && errno != ESRCH) {
                    fprintf(stderr, "*** error: parent kill(%u, SIGTERM): %s\n", service_pid, strerror(errno));
                }

                for (;;) {
                    int child_status = 0;
                    int result = waitpid(service_pid, &child_status, 0);
                    if (result < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno != ECHILD) {
                            fprintf(stderr, "*** error: parent waitpid(%u, &status, 0): %s\n", service_pid, strerror(errno));
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
                [POLLFD_PIPE] = { pipefd[PIPE_READ], POLLIN, 0 },
            };

            while (pollfds[POLLFD_PID].events != 0 || pollfds[POLLFD_PIPE].events != 0) {
                pollfds[POLLFD_PID ].revents = 0;
                pollfds[POLLFD_PIPE].revents = 0;

                int result = poll(pollfds, 2, -1);
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    fprintf(stderr, "*** error: parent poll(&pollfds, 2, -1): %s\n", strerror(errno));
                    break;
                }

                if (pollfds[POLLFD_PIPE].revents & POLLIN) {
                    if (logfile_has_format) {
                        // logrotate
                        time_t now = time(NULL);
                        struct tm local_now;
                        if (localtime_r(&now, &local_now) == NULL) {
                            fprintf(stderr, "*** error: parent getting local time: %s\n", strerror(errno));
                            status = 1;
                            goto cleanup;
                        }

                        if (strftime(new_logfile_path, sizeof(new_logfile_path), logfile, &local_now) == 0) {
                            fprintf(stderr, "*** error: parent cannot format logfile \"%s\": %s\n", logfile, strerror(errno));
                            status = 1;
                            goto cleanup;
                        }

                        if (strcmp(new_logfile_path, logfile_path) != 0) {
                            int new_logfile_fd = open(new_logfile_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
                            if (logfile_fd == -1) {
                                fprintf(stderr, "*** error: parent cannot open logfile: %s: %s\n", new_logfile_path, strerror(errno));
                            }

                            if (close(logfile_fd) != 0) {
                                fprintf(stderr, "*** error: parent close(logfile_fd): %s\n", strerror(errno));
                            }

                            logfile_fd = new_logfile_fd;
                            if (dup2(logfile_fd, STDOUT_FILENO) != 0) {
                                fprintf(stderr, "*** error: parent dup2(logfile_fd, STDOUT_FILENO): %s\n", strerror(errno));
                            }

                            if (dup2(logfile_fd, STDERR_FILENO) != 0) {
                                fprintf(stderr, "*** error: parent dup2(logfile_fd, STDERR_FILENO): %s\n", strerror(errno));
                            }

                            strcpy(logfile_path, new_logfile_path);
                        }
                    }

                    // handle log messages
                    ssize_t count = splice(pipefd[PIPE_READ], NULL, logfile_fd, NULL, SPLICE_SZIE, SPLICE_F_NONBLOCK);
                    if (count < 0) {
                        fprintf(stderr, "*** error: parent splice(pipefd[PIPE_READ], NULL, logfile_fd, NULL, SPLICE_SZIE, SPLICE_F_NONBLOCK): %s\n", strerror(errno));
                    }
                }

                if (pollfds[POLLFD_PIPE].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    pollfds[POLLFD_PIPE].events = 0;
                }

                if (pollfds[POLLFD_PID].revents & POLLIN) {
                    siginfo_t siginfo;
                    int result = waitid(P_PIDFD, service_pidfd, &siginfo, WNOHANG);
                    if (result == 0) {
                        // would have blocked
                    } else if (result == -1) {
                        pollfds[POLLFD_PIPE].events = 0;
                        fprintf(stderr, "*** error: parent waitid(P_PIDFD, pidfd, &siginfo, WNOHANG): %s\n", strerror(errno));
                    } else {
                        pollfds[POLLFD_PIPE].events = 0;
                        bool crash = false;
                        switch (siginfo.si_code) {
                            case CLD_EXITED:
                                if (siginfo.si_status == 0) {
                                    printf("%s exited normally\n", name);
                                } else {
                                    fprintf(stderr, "*** error: %s exited with error status %d\n", name, siginfo.si_status);
                                    crash = true;
                                }
                                break;

                            case CLD_KILLED:
                                fprintf(stderr, "*** error: %s was killed by signal %d\n", name, siginfo.si_status);
                                crash = true;

                                switch (siginfo.si_status) {
                                    case SIGTERM:
                                    case SIGINT:
                                    case SIGKILL:
                                        printf("service stopped via signal %d -> don't restart\n", siginfo.si_status);
                                        running = false;
                                        break;
                                }
                                break;

                            case CLD_DUMPED:
                                fprintf(stderr, "*** error: %s was killed by signal %d and dumped core\n", name, siginfo.si_status);
                                crash = true;
                                break;

                            case CLD_STOPPED:
                            case CLD_TRAPPED:
                            case CLD_CONTINUED:
                                assert(false);
                                break;

                            default:
                                assert(false);
                                break;
                        }

                        if (crash) {
                            struct timespec ts_before;
                            struct timespec ts_after;
                            bool time_ok = true;

                            if (crash_report == NULL) {
                                time_ok = false;
                            } else {
                                if (clock_gettime(CLOCK_MONOTONIC, &ts_before) != 0) {
                                    time_ok = false;
                                    fprintf(stderr, "*** error: parent clock_gettime(CLOCK_MONOTONIC, &ts_before): %s\n", strerror(errno));
                                }

                                const char *code_str =
                                    siginfo.si_code == CLD_KILLED ? "KILLED" :
                                    siginfo.si_code == CLD_DUMPED ? "DUMPED" :
                                                                    "EXITED";
                                char status_str[24];

                                int result = snprintf(status_str, sizeof(status_str), "%d", siginfo.si_status);
                                assert(result > 0 && result <= sizeof(status_str));

                                pid_t report_pid = 0;
                                result = posix_spawn(&report_pid, crash_report, NULL, NULL,
                                    (char*[]){ (char*)crash_report, (char*)code_str, status_str, logfile_path, NULL },
                                    environ);

                                if (result != 0) {
                                    fprintf(stderr, "*** error: parent starting crash reporter: %s\n", strerror(errno));
                                } else {
                                    for (;;) {
                                        int report_status = 0;
                                        int result = waitpid(report_pid, &report_status, 0);
                                        if (result < 0) {
                                            if (errno == EINTR) {
                                                continue;
                                            }
                                            fprintf(stderr, "*** error: parent waitpid(%u, &report_status, 0): %s\n", report_pid, strerror(errno));
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
                                    fprintf(stderr, "*** error: parent clock_gettime(CLOCK_MONOTONIC, &ts_after): %s\n", strerror(errno));
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
                        } else {
                            running = false;
                        }
                    }
                }

                if (pollfds[POLLFD_PID].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    pollfds[POLLFD_PID].events = 0;
                }
            }

            if (close(pipefd[PIPE_READ]) != 0) {
                fprintf(stderr, "*** error: parent close(pipefd[PIPE_READ]): %s\n", strerror(errno));
            }

            if (close(service_pidfd) != 0) {
                fprintf(stderr, "*** error: parent close(pidfd): %s\n", strerror(errno));
            }
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

const struct option stop_options[] = {
    [OPT_STOP_PIDFILE]          = { "pidfile", required_argument, 0, 'p' },
    [OPT_STOP_SHUTDOWN_TIMEOUT] = { "shutdown-timeout", required_argument, 0, 0 },
    { 0, 0, 0, 0 },
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

    size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
    pidfile_runner = malloc(pidfile_runner_size);
    if (pidfile_runner == NULL) {
        fprintf(stderr, "*** error: malloc: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile) != pidfile_runner_size - 1) {
        assert(false);
    }

    pid_t runner_pid = 0;
    bool pidfile_runner_ok = read_pidfile(pidfile_runner, &runner_pid) == 0;
    bool pidfile_ok        = read_pidfile(pidfile, &service_pid) == 0;

    pid_t pid = 0;
    if (pidfile_runner_ok) {
        if (kill(runner_pid, SIGTERM) != 0) {
            fprintf(stderr, "*** error: sending SIGTERM to service-runner PID %d: %s\n", runner_pid, strerror(errno));
            status = 1;
            goto cleanup;
        }

        pid = runner_pid;
    } else if (pidfile_ok) {
        if (kill(service_pid, SIGTERM) != 0) {
            fprintf(stderr, "*** error: sending SIGTERM to service PID %d: %s\n", service_pid, strerror(errno));
            status = 1;
            goto cleanup;
        }

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
        int opt = getopt_long(argc - 1, argv + 1, "p:", stop_options, NULL);

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

    size_t pidfile_runner_size = strlen(pidfile) + strlen(".runner") + 1;
    pidfile_runner = malloc(pidfile_runner_size);
    if (pidfile_runner == NULL) {
        fprintf(stderr, "*** error: malloc: %s\n", strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile) != pidfile_runner_size - 1) {
        assert(false);
    }

    pid_t runner_pid = 0;
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

int command_help(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    if (argc == 2) {
        usage(argc, argv);
        return 0;
    }

    if (argc != 3) {
        fprintf(stderr, "*** error: illegal number of arguments\n");
        short_usage(argc, argv);
        return 1;
    }

    const char *command = argv[2];
    if (strcmp(command, "start") == 0) {
        printf("\n" HELP_CMD_START, get_progname(argc, argv));
        return 0;
    } else if (strcmp(command, "stop") == 0) {
        printf("\n" HELP_CMD_STOP, get_progname(argc, argv));
        return 0;
    } else if (strcmp(command, "status") == 0) {
        printf("\n" HELP_CMD_STATUS, get_progname(argc, argv));
        return 0;
    } else if (strcmp(command, "help") == 0) {
        printf("\n" HELP_CMD_HELP, get_progname(argc, argv));
        return 0;
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        short_usage(argc, argv);
        return 1;
    }
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
        return command_help(argc, argv);
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        usage(argc, argv);
        return 1;
    }

    return 0;
}
