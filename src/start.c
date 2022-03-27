#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wait.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
// #include <sys/prctl.h>
// #include <linux/capability.h>
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
#include <inttypes.h>

#include "service-runner.h"

#ifndef P_PIDFD
    #define P_PIDFD 3
#endif

#define PIPE_READ  0
#define PIPE_WRITE 1
#define SPLICE_SZIE ((size_t)2 * 1024 * 1024 * 1024)

#define LOG_LEVEL_UPPER_INFO_STR  "INFO"
#define LOG_LEVEL_UPPER_ERROR_STR "ERROR"

#define LOG_LEVEL_LOWER_INFO_STR  "info"
#define LOG_LEVEL_LOWER_ERROR_STR "error"

#define LOG_LEVEL_INFO_LEN  4
#define LOG_LEVEL_ERROR_LEN 5

enum LogLevel {
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_ERROR = 2,
};

extern char **environ;

// static int cap_get_bound(int cap) {
//     int result = prctl(PR_CAPBSET_READ, (unsigned long) cap, (unsigned long) 0);
//     if (result < 0) {
//         errno = -result;
//         return -1;
//     }
// 
//     return result;
// }

struct rlimit_params {
    int resource;
    struct rlimit limit;
};

// Currently there are only 16 different rlimit values anyway.
#define RLIMITS_GROW ((size_t) 16)

static bool parse_rlimit_params(const char *arg, struct rlimit_params *limitptr) {
    char *endptr = strchr(arg, ':');
    if (endptr == NULL) {
        errno = EINVAL;
        return false;
    }
    const size_t reslen = endptr - arg;
    char buf[16];
    if (reslen >= sizeof(buf)) {
        errno = EINVAL;
        return false;
    }

    const char *arg1 = endptr + 1;

    endptr = NULL;
    long long rlim_cur = strtoll(arg1, &endptr, 10);
    if (!*arg1 || (*endptr != ':' && *endptr != 0)) {
        return false;
    }

    if (endptr == arg1) {
        endptr = strchr(arg1, ':');
        if (endptr == NULL) {
            if (strcasecmp(arg1, "INF") != 0 && strcasecmp(arg1, "INFINITY") != 0) {
                errno = EINVAL;
                return false;
            }
            endptr = (char*)arg + strlen(arg);
        } else {
            const size_t len = endptr - arg1;
            if (len >= sizeof(buf)) {
                errno = EINVAL;
                return false;
            }

            memcpy(buf, arg1, len);
            buf[len] = 0;

            if (strcasecmp(arg1, "INF") != 0 && strcasecmp(arg1, "INFINITY") != 0) {
                errno = EINVAL;
                return false;
            }
        }

        rlim_cur = RLIM_INFINITY;
    }

    bool has_max = *endptr == ':';
    long long rlim_max = RLIM_INFINITY;
    if (has_max) {
        const char *arg2 = endptr + 1;
        if (strcasecmp(arg2, "INF") != 0 && strcasecmp(arg2, "INFINITY") != 0) {
            endptr = NULL;
            rlim_max = strtoll(arg2, &endptr, 10);
            if (!*arg2 || *endptr) {
                return false;
            }
        }
    }

    memcpy(buf, arg, reslen);
    buf[reslen] = 0;

    int resource = -1;
    if (strcasecmp(buf, "AS") == 0) {
        resource = RLIMIT_AS;
    } else if (strcasecmp(buf, "CORE") == 0) {
        resource = RLIMIT_CORE;
    } else if (strcasecmp(buf, "CPU") == 0) {
        resource = RLIMIT_CPU;
    } else if (strcasecmp(buf, "DATA") == 0) {
        resource = RLIMIT_DATA;
    } else if (strcasecmp(buf, "FSIZE") == 0) {
        resource = RLIMIT_FSIZE;
    } else if (strcasecmp(buf, "LOCKS") == 0) {
        resource = RLIMIT_LOCKS;
    } else if (strcasecmp(buf, "MEMLOCK") == 0) {
        resource = RLIMIT_MEMLOCK;
    } else if (strcasecmp(buf, "MSGQUEUE") == 0) {
        resource = RLIMIT_MSGQUEUE;
#ifdef RLIMIT_NICE
    } else if (strcasecmp(buf, "NICE") == 0) {
        resource = RLIMIT_NICE;
#endif
    } else if (strcasecmp(buf, "NOFILE") == 0) {
        resource = RLIMIT_NOFILE;
    } else if (strcasecmp(buf, "NPROC") == 0) {
        resource = RLIMIT_NPROC;
    } else if (strcasecmp(buf, "RSS") == 0) {
        resource = RLIMIT_RSS;
#ifdef RLIMIT_RTPRIO
    } else if (strcasecmp(buf, "RTPRIO") == 0) {
        resource = RLIMIT_RTPRIO;
#endif
#ifdef RLIMIT_RTTIME
    } else if (strcasecmp(buf, "RTTIME") == 0) {
        resource = RLIMIT_RTTIME;
#endif
#ifdef RLIMIT_SIGPENDING
    } else if (strcasecmp(buf, "SIGPENDING") == 0) {
        resource = RLIMIT_SIGPENDING;
#endif
#ifdef RLIMIT_STACK
    } else if (strcasecmp(buf, "STACK") == 0) {
        resource = RLIMIT_STACK;
#endif
    } else {
        endptr = NULL;
        unsigned long value = strtoul(arg, &endptr, 10);
        if (*endptr != ':' || endptr == arg || value > INT_MAX) {
            errno = EINVAL;
            return false;
        }
        resource = value;
    }

    if (!has_max) {
        struct rlimit lim;
        if (getrlimit(resource, &lim) != 0) {
            return false;
        }
        rlim_max = lim.rlim_max;
    }

    if (limitptr != NULL) {
        limitptr->resource = resource;
        limitptr->limit.rlim_cur = rlim_cur;
        limitptr->limit.rlim_max = rlim_max;
    }

    return true;
}

enum {
    OPT_START_PIDFILE,
    OPT_START_LOGFILE,
    OPT_START_CHOWN_LOGFILE,
    OPT_START_LOG_FORMAT,
    OPT_START_USER,
    OPT_START_GROUP,
    OPT_START_PRIORITY,
    OPT_START_RLIMIT,
    OPT_START_UMASK,
    OPT_START_CHROOT,
    OPT_START_CHDIR,
    // TODO: --procsched and --iosched?
    OPT_START_RESTART,
    OPT_START_CRASH_REPORT,
    OPT_START_RESTART_SLEEP,
    OPT_START_COUNT,
};

static const struct option start_options[] = {
    [OPT_START_PIDFILE]          = { "pidfile",          required_argument, 0, 'p' },
    [OPT_START_LOGFILE]          = { "logfile",          required_argument, 0, 'l' },
    [OPT_START_CHOWN_LOGFILE]    = { "chown-logfile",    no_argument,       0,  0  },
    [OPT_START_LOG_FORMAT]       = { "log-format",       required_argument, 0,  0  },
    [OPT_START_USER]             = { "user",             required_argument, 0, 'u' },
    [OPT_START_GROUP]            = { "group",            required_argument, 0, 'g' },
    [OPT_START_PRIORITY]         = { "priority",         required_argument, 0, 'N' },
    [OPT_START_RLIMIT]           = { "rlimit",           required_argument, 0, 'r' },
    [OPT_START_UMASK]            = { "umask",            required_argument, 0, 'k' },
    [OPT_START_CHROOT]           = { "chroot",           required_argument, 0,  0  },
    [OPT_START_CHDIR]            = { "chdir",            required_argument, 0, 'C' },
    [OPT_START_RESTART]          = { "restart",          required_argument, 0,  0  },
    [OPT_START_CRASH_REPORT]     = { "crash-report",     required_argument, 0,  0  },
    [OPT_START_RESTART_SLEEP]    = { "restart-sleep",    required_argument, 0,  0  },
    [OPT_START_COUNT]            = { 0, 0, 0, 0 },
};

enum Restart {
    RESTART_NEVER   = 0,
    RESTART_ALWAYS  = 1,
    RESTART_FAILURE = 2,
};

enum LogFormat {
    LOG_FORMAT_TEXT = 0,
    LOG_FORMAT_JSON = 1,
};

static const char *log_format = LOG_TEMPLATE_TEXT;
static pid_t service_pid = 0;
static int service_pidfd = -1;
static volatile bool running = false;
static volatile bool restart_issued = false;
static volatile bool got_sigchld = false;

#if !defined(__GNUC__) && !defined(__clang__)
    #define __attribute__(X)
#endif

static void print_json_string(FILE *fp, const char *str) {
    const char *prev = str;
    for (const char *ptr = str;;) {
        char ch = *ptr;
        switch (ch) {
        case 0:
            fwrite(prev, ptr - prev, 1, fp);
            return;

        case '\\':
        case '"':
        case '/':
            fwrite(prev, ptr - prev, 1, fp);
            fputc('\\', fp);
            fputc(ch, fp);
            prev = ++ ptr;
            break;

        case '\r':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\r", 2, 1, fp);
            prev = ++ ptr;
            break;

        case '\n':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\n", 2, 1, fp);
            prev = ++ ptr;
            break;

        case '\t':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\t", 2, 1, fp);
            prev = ++ ptr;
            break;

        case '\b':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\b", 2, 1, fp);
            prev = ++ ptr;
            break;

        case '\f':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\f", 2, 1, fp);
            prev = ++ ptr;
            break;

        case '<':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\u003c", 6, 1, fp);
            prev = ++ ptr;
            break;

        case '>':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\\u003e", 6, 1, fp);
            prev = ++ ptr;
            break;

        default:
            ++ ptr;
            break;
        }
    }
}

static void print_xml_string(FILE *fp, const char *str) {
    const char *prev = str;
    for (const char *ptr = str;;) {
        char ch = *ptr;
        switch (ch) {
        case 0:
            fwrite(prev, ptr - prev, 1, fp);
            return;

        case '&':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&amp;", 5, 1, fp);
            prev = ++ ptr;
            break;

        case '"':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&quot;", 6, 1, fp);
            prev = ++ ptr;
            break;

        case '\'':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&#39;", 5, 1, fp);
            prev = ++ ptr;
            break;

        case '<':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&lt;", 4, 1, fp);
            prev = ++ ptr;
            break;

        case '>':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&gt;", 4, 1, fp);
            prev = ++ ptr;
            break;

        case '\r':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&#13;", 5, 1, fp);
            prev = ++ ptr;
            break;

        case '\n':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("&#10;", 5, 1, fp);
            prev = ++ ptr;
            break;

        default:
            ++ ptr;
            break;
        }
    }
}

static void print_sql_string(FILE *fp, const char *str) {
    const char *prev = str;
    for (const char *ptr = str;;) {
        char ch = *ptr;
        switch (ch) {
        case 0:
            fwrite(prev, ptr - prev, 1, fp);
            return;

        case '\'':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("''", 2, 1, fp);
            prev = ++ ptr;
            break;

        default:
            ++ ptr;
            break;
        }
    }
}

static void print_csv_string(FILE *fp, const char *str) {
    const char *prev = str;
    for (const char *ptr = str;;) {
        char ch = *ptr;
        switch (ch) {
        case 0:
            fwrite(prev, ptr - prev, 1, fp);
            return;

        case '"':
            fwrite(prev, ptr - prev, 1, fp);
            fwrite("\"\"", 2, 1, fp);
            prev = ++ ptr;
            break;

        default:
            ++ ptr;
            break;
        }
    }
}

static bool is_valid_log_template(const char *template) {
    bool ok = false;
    for (const char *ptr = template; *ptr; ++ ptr) {
        char ch = *ptr;
        if (ch == '%') {
            ch = * ++ ptr;
            switch (ch) {
                case 's':
                    ok = true;
                    break;

                case 'j':
                case 'x':
                case 'q':
                case 'c':
                    ch = * ++ ptr;
                    switch (ch) {
                        case 's':
                            ok = true;
                            break;

                        case 'f':
                        case 'l':
                        case 'L':
                            break;

                        default:
                            return false;
                    }
                    break;

                case 'g':
                    ch = * ++ ptr;
                    switch (ch) {
                        case 'Y':
                        case 'm':
                        case 'd':
                        case 'H':
                        case 'M':
                        case 'S':
                        case 't':
                        case 'T':
                        case 'a':
                        case 'b':
                            break;

                        default:
                            return false;
                    }
                    break;

                case 'Y':
                case 'm':
                case 'd':
                case 'H':
                case 'M':
                case 'S':
                case 'z':
                case 't':
                case 'T':
                case 'f':
                case 'n':
                case 'l':
                case 'L':
                case 'h':
                case 'a':
                case 'b':
                case '%':
                    break;

                default:
                    return false;
            }
        }
    }

    return ok;
}

static const char *wdays[] = {
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
};

static const char *months[] = {
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
};

#define FORMAT_ARG(FMT, PRINT)                  \
    ch = *ptr;                                  \
    if (ch == 0) {                              \
        fwrite(FMT, 2, 1, fp);                  \
        break;                                  \
    }                                           \
    switch (ch) {                               \
        case 's':                               \
            PRINT(fp, msg);                     \
            prev = ++ ptr;                      \
            break;                              \
                                                \
        case 'f':                               \
            PRINT(fp, filename);                \
            prev = ++ ptr;                      \
            break;                              \
                                                \
        case 'l':                               \
            PRINT(fp, level == LOG_LEVEL_INFO ? \
                LOG_LEVEL_LOWER_INFO_STR :      \
                LOG_LEVEL_LOWER_ERROR_STR);     \
            prev = ++ ptr;                      \
            break;                              \
                                                \
        case 'L':                               \
            PRINT(fp, level == LOG_LEVEL_INFO ? \
                LOG_LEVEL_UPPER_INFO_STR :      \
                LOG_LEVEL_UPPER_ERROR_STR);     \
            prev = ++ ptr;                      \
            break;                              \
                                                \
        default:                                \
            fwrite(FMT, 2, 1, fp);              \
            break;                              \
    }

__attribute__((format(printf, 6, 7))) static void print_log_template(FILE *fp, const char *template, enum LogLevel level, const char *filename, size_t lineno, const char *fmt, ...) {
    char buf[4096];
    const char *msg = buf;
    bool free_msg = false;
    const time_t now = time(NULL);
    struct tm local_now = {
        .tm_year   = -1900,
        .tm_mon    = 0,
        .tm_mday   = 0,
        .tm_wday   = 0,
        .tm_hour   = 0,
        .tm_min    = 0,
        .tm_sec    = 0,
        .tm_isdst  = 0,
        .tm_gmtoff = 0,
        .tm_zone   = NULL,
    };

    struct tm *tmptr = localtime_r(&now, &local_now);
    assert(tmptr != NULL); (void)tmptr;

    va_list ap;
    va_start(ap, fmt);
    int count = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (count >= 0) {
        if (count < sizeof(buf)) {
            msg = buf;
        } else {
            size_t size = (size_t)count + 1;
            char *buf = malloc(size);
            if (buf == NULL) {
                msg = strerror(errno);
            } else {
                free_msg = true;
                va_start(ap, fmt);
                count = vsnprintf(buf, size, fmt, ap);
                assert(count >= 0 && (size_t)count < size);
                va_end(ap);
            }
        }
    } else {
        msg = strerror(errno);
    }

    int tzoff = local_now.tm_gmtoff / 60;
    char tzsign;
    if (tzoff < 0) {
        tzsign = '-';
        tzoff  = -tzoff;
    } else {
        tzsign = '+';
    }
    int tzhour = tzoff / 60;
    int tzmin  = tzoff % 60;

    const char *prev = template;
    for (const char *ptr = template;;) {
        char ch = *ptr;

        if (ch == 0) {
            fwrite(prev, ptr - prev, 1, fp);
            break;
        } else if (ch == '%') {
            fwrite(prev, ptr - prev, 1, fp);
            ++ ptr;
            ch = *ptr;
            if (ch == 0) {
                fputc('%', fp);
                break;
            }
            prev = ++ ptr;
            switch (ch) {
                case 'Y':
                    fprintf(fp, "%04d", local_now.tm_year + 1900);
                    break;

                case 'm':
                    fprintf(fp, "%02d", local_now.tm_mon + 1);
                    break;

                case 'd':
                    fprintf(fp, "%02d", local_now.tm_mday);
                    break;

                case 'H':
                    fprintf(fp, "%02d", local_now.tm_hour);
                    break;

                case 'M':
                    fprintf(fp, "%02d", local_now.tm_min);
                    break;

                case 'S':
                    fprintf(fp, "%02d", local_now.tm_sec);
                    break;

                case 'z':
                    fprintf(fp, "%c%02d%02d", tzsign, tzhour, tzmin);
                    break;

                case 't':
                    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d%c%02d%02d",
                        local_now.tm_year + 1900,
                        local_now.tm_mon + 1,
                        local_now.tm_mday,
                        local_now.tm_hour,
                        local_now.tm_min,
                        local_now.tm_sec,
                        tzsign,
                        tzhour,
                        tzmin
                    );
                    break;

                case 'T':
                    fprintf(fp, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d%02d",
                        local_now.tm_year + 1900,
                        local_now.tm_mon + 1,
                        local_now.tm_mday,
                        local_now.tm_hour,
                        local_now.tm_min,
                        local_now.tm_sec,
                        tzsign,
                        tzhour,
                        tzmin
                    );
                    break;

                case 's':
                    fwrite(msg, strlen(msg), 1, fp);
                    break;

                case 'j':
                    FORMAT_ARG("%j", print_json_string);
                    break;

                case 'x':
                    FORMAT_ARG("%x", print_xml_string);
                    break;

                case 'q':
                    FORMAT_ARG("%q", print_sql_string);
                    break;

                case 'c':
                    FORMAT_ARG("%c", print_csv_string);
                    break;

                case 'f':
                    fwrite(filename, strlen(filename), 1, fp);
                    break;

                case 'n':
                    fprintf(fp, "%zu", lineno);
                    break;

                case 'l':
                    if (level == LOG_LEVEL_INFO) {
                        fwrite(LOG_LEVEL_LOWER_INFO_STR, LOG_LEVEL_INFO_LEN, 1, fp);
                    } else {
                        fwrite(LOG_LEVEL_LOWER_ERROR_STR, LOG_LEVEL_ERROR_LEN, 1, fp);
                    }
                    break;

                case 'L':
                    if (level == LOG_LEVEL_INFO) {
                        fwrite(LOG_LEVEL_UPPER_INFO_STR, LOG_LEVEL_INFO_LEN, 1, fp);
                    } else {
                        fwrite(LOG_LEVEL_UPPER_ERROR_STR, LOG_LEVEL_ERROR_LEN, 1, fp);
                    }
                    break;

                case 'h':
                {
                    struct tm gm_now = {
                        .tm_year   = -1900,
                        .tm_mon    = 0,
                        .tm_mday   = 0,
                        .tm_wday   = 0,
                        .tm_hour   = 0,
                        .tm_min    = 0,
                        .tm_sec    = 0,
                        .tm_isdst  = 0,
                        .tm_gmtoff = 0,
                        .tm_zone   = NULL,
                    };
                    struct tm *tmptr = gmtime_r(&now, &gm_now);
                    assert(tmptr != NULL); (void)tmptr;
                    fprintf(fp, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                        wdays[gm_now.tm_wday],
                        gm_now.tm_mday,
                        months[gm_now.tm_mon],
                        gm_now.tm_year + 1900,
                        gm_now.tm_hour,
                        gm_now.tm_min,
                        gm_now.tm_sec
                    );
                    break;
                }
                case 'g':
                {
                    struct tm gm_now = {
                        .tm_year   = -1900,
                        .tm_mon    = 0,
                        .tm_mday   = 0,
                        .tm_wday   = 0,
                        .tm_hour   = 0,
                        .tm_min    = 0,
                        .tm_sec    = 0,
                        .tm_isdst  = 0,
                        .tm_gmtoff = 0,
                        .tm_zone   = NULL,
                    };
                    ch = *ptr;
                    if (ch == 0) {
                        fwrite("%g", 2, 1, fp);
                        break;
                    }
                    struct tm *tmptr = gmtime_r(&now, &gm_now);
                    assert(tmptr != NULL); (void)tmptr;
                    switch (ch) {
                        case 'Y':
                            fprintf(fp, "%04d", gm_now.tm_year + 1900);
                            prev = ++ ptr;
                            break;

                        case 'm':
                            fprintf(fp, "%02d", gm_now.tm_mon + 1);
                            prev = ++ ptr;
                            break;

                        case 'd':
                            fprintf(fp, "%02d", gm_now.tm_mday);
                            prev = ++ ptr;
                            break;

                        case 'H':
                            fprintf(fp, "%02d", gm_now.tm_hour);
                            prev = ++ ptr;
                            break;

                        case 'M':
                            fprintf(fp, "%02d", gm_now.tm_min);
                            prev = ++ ptr;
                            break;

                        case 'S':
                            fprintf(fp, "%02d", gm_now.tm_sec);
                            prev = ++ ptr;
                            break;

                        case 't':
                            fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02dZ",
                                gm_now.tm_year + 1900,
                                gm_now.tm_mon + 1,
                                gm_now.tm_mday,
                                gm_now.tm_hour,
                                gm_now.tm_min,
                                gm_now.tm_sec
                            );
                            prev = ++ ptr;
                            break;

                        case 'T':
                            fprintf(fp, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                gm_now.tm_year + 1900,
                                gm_now.tm_mon + 1,
                                gm_now.tm_mday,
                                gm_now.tm_hour,
                                gm_now.tm_min,
                                gm_now.tm_sec
                            );
                            prev = ++ ptr;
                            break;

                        case 'a':
                        {
                            const char *mon = months[gm_now.tm_mon];
                            fwrite(mon, strlen(mon), 1, fp);
                            prev = ++ ptr;
                            break;
                        }
                        case 'b':
                        {
                            const char *wday = wdays[gm_now.tm_wday];
                            fwrite(wday, strlen(wday), 1, fp);
                            prev = ++ ptr;
                            break;
                        }
                        default:
                            fwrite("%g", 2, 1, fp);
                            break;
                    }
                    break;
                }
                case 'a':
                {
                    const char *mon = months[local_now.tm_mon];
                    fwrite(mon, strlen(mon), 1, fp);
                    break;
                }
                case 'b':
                {
                    const char *wday = wdays[local_now.tm_wday];
                    fwrite(wday, strlen(wday), 1, fp);
                    break;
                }
                case '%':
                    fputc('%', fp);
                    break;

                default:
                    fwrite(ptr - 2, 2, 1, fp);
                    break;
            }
        } else {
            ++ ptr;
        }
    }

    fputc('\n', fp);

    if (free_msg) {
        free((char*)msg);
    }
}

#define print_info(FMT, ...)  print_log_template(stdout, log_format, LOG_LEVEL_INFO,  __FILE__, __LINE__, FMT, ## __VA_ARGS__)
#define print_error(FMT, ...) print_log_template(stdout, log_format, LOG_LEVEL_ERROR, __FILE__, __LINE__, FMT, ## __VA_ARGS__)

static bool is_valid_name(const char *name) {
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

static bool can_execute(const char *filename, uid_t uid, gid_t gid) {
    struct stat meta;

    if (stat(filename, &meta) != 0) {
        return false;
    }

    if (S_ISDIR(meta.st_mode)) {
        errno = EISDIR;
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

static bool can_list(const char *dirname, uid_t uid, gid_t gid) {
    struct stat meta;

    if (stat(dirname, &meta) != 0) {
        return false;
    }

    if (!S_ISDIR(meta.st_mode)) {
        errno = ENOTDIR;
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

static bool can_read_write(const char *filename, uid_t uid, gid_t gid) {
    struct stat meta;

    if (stat(filename, &meta) != 0) {
        if (errno == ENOENT) {
            if (uid == 0 || gid == 0) {
                // root
                return true;
            }

            char *namedup = strdup(filename);
            if (namedup == NULL) {
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

    if (S_ISDIR(meta.st_mode)) {
        errno = EISDIR;
        return false;
    }

    if (uid == 0 || gid == 0) {
        // root
        return true;
    }

    bool can_read  = false;
    bool can_write = false;

    if (meta.st_mode & S_IROTH) {
        can_read = true;
    } else if (meta.st_gid == gid && meta.st_mode & S_IRGRP) {
        can_read = true;
    } else if (meta.st_uid == uid && meta.st_mode & S_IRUSR) {
        can_read = true;
    }

    if (meta.st_mode & S_IWOTH) {
        can_write = true;
    } else if (meta.st_gid == gid && meta.st_mode & S_IWGRP) {
        can_write = true;
    } else if (meta.st_uid == uid && meta.st_mode & S_IWUSR) {
        can_write = true;
    }

    if (can_read && can_write) {
        return true;
    }

    errno = EACCES;
    return false;
}

static void signal_premature_exit(pid_t runner_pid) {
    // This attempts to stop the service-runner process so that an crash-restart-loop
    // is prevented if the service process doesn't even manage to exec.
    print_error("(child) premature exit before execv() or failed execv() -> don't restart");
    if (kill(runner_pid, SIGTERM) != 0) {
        print_error("(child) signaling premature exit to service-runner: %s",
            strerror(errno));
    }
}

static int get_uid_from_name(const char *username, uid_t *uidptr) {
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

static int get_gid_from_name(const char *groupname, gid_t *gidptr) {
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

static void handles_stop_signal(int sig) {
    if (service_pid == 0) {
        print_error("received signal %d, but service process is not running -> ignored", sig);
        return;
    }

    assert(getpid() != service_pid);

    print_info("received signal %d, forwarding to service PID %u", sig, service_pid);
    running = false;

    if (service_pidfd != -1) {
        if (pidfd_send_signal(service_pidfd, sig, NULL, 0) != 0) {
            if (errno == EBADFD || errno == ENOSYS) {
                print_error("pidfd_send_signal(service_pidfd, %d, NULL, 0) failed, using kill(%d, %d): %s",
                    sig, service_pid, sig, strerror(errno));
                if (service_pid != 0 && kill(service_pid, sig) != 0) {
                    print_error("forwarding signal %d to PID %d: %s", sig, service_pid, strerror(errno));
                }
            } else {
                print_error("forwarding signal %d to PID %d via pidfd: %s", sig, service_pid, strerror(errno));
            }
        }
    } else if (kill(service_pid, sig) != 0) {
        print_error("forwarding signal %d to PID %d: %s", sig, service_pid, strerror(errno));
    }
}

static void handle_restart_signal(int sig) {
    if (service_pid == 0) {
        print_error("received signal %d, but service process is not running -> ignored", sig);
        return;
    }

    assert(getpid() != service_pid);

    print_info("received signal %d, restarting service...", sig);
    restart_issued = true;

    if (service_pidfd != -1) {
        if (pidfd_send_signal(service_pidfd, SIGTERM, NULL, 0) != 0) {
            if (errno == EBADFD || errno == ENOSYS) {
                print_error("pidfd_send_signal(%d, SIGTERM, NULL, 0) failed, using kill(%d, SIGTERM): %s",
                    service_pidfd, service_pid, strerror(errno));
                if (service_pid != 0 && kill(service_pid, SIGTERM) != 0) {
                    print_error("sending SIGTERM to PID %d: %s", service_pid, strerror(errno));
                }
            } else {
                print_error("sending SIGTERM to PID %d via pidfd: %s", service_pid, strerror(errno));
            }
        }
    } else if (kill(service_pid, SIGTERM) != 0) {
        print_error("sending SIGTERM to PID %d: %s", service_pid, strerror(errno));
    }
}

static void handle_child(int sig) {
    // for when pidfd is not supported handle child exiting via SIGCHLD and EINTR of poll()
    got_sigchld = true;
}

int command_start(int argc, char *argv[]) {
    if (argc < 2) {
        return 1;
    }

    // Ensure that stdout is line buffered, no matter if it is a tty.
    // This is so that lines appear correctly in the logfile.
    // This needs to happen before any write to stdout.
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        perror("*** error: setvbuf(stdout, NULL, _IOLBF, 0)");
        return 1;
    }

    // Also set stderr to be line buffered so that concurrent writes
    // between the service-runner process and the service itself have
    // a smaller chance to interfere.
    // This is ok since log messages as written here are always single
    // and whole lines (assuming stderr(errno) never returns a
    // multiline string).
    if (setvbuf(stderr, NULL, _IOLBF, 0) != 0) {
        perror("*** error: setvbuf(stderr, NULL, _IOLBF, 0)");
        return 1;
    }

    int longind = 0;

    const char *pidfile = NULL;
    const char *logfile = NULL;
    const char *user    = NULL;
    const char *group   = NULL;
    const char *chdir_path = NULL;
    char *chroot_path = NULL;

    bool chown_logfile = false;
    const char *crash_report = NULL;
    unsigned int restart_sleep = 1;

    enum Restart restart = RESTART_FAILURE;

    bool set_priority = false;
    bool set_umask    = false;
    int priority    = 0;
    int umask_value = 0;

    struct rlimit_params *rlimits = NULL;
    size_t rlimits_capacity = 0;
    size_t rlimits_count    = 0;

    int status = 0;
    int logfile_fd = -1;
    int pipefd[2] = { -1, -1 };

    bool free_pidfile = false;
    bool free_logfile = false;
    bool free_command = false;
    bool free_chdir_path  = false;
    bool cleanup_pidfiles = false;
    bool rlimit_fsize = false;

    char *pidfile_runner = NULL;
    char logfile_path_buf[PATH_MAX];

    for (;;) {
        int opt = getopt_long(argc - 1, argv + 1, "p:l:u:g:N:k:r:C:", start_options, &longind);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 0:
                switch (longind) {
                    case OPT_START_CHOWN_LOGFILE:
                        chown_logfile = true;
                        break;

                    case OPT_START_LOG_FORMAT:
                        if (strcasecmp(optarg, "text") == 0) {
                            log_format = LOG_TEMPLATE_TEXT;
                        } else if (strcasecmp(optarg, "json") == 0) {
                            log_format = LOG_TEMPLATE_JSON;
                        } else if (strcasecmp(optarg, "xml") == 0) {
                            log_format = LOG_TEMPLATE_XML;
                        } else if (strcasecmp(optarg, "sql") == 0) {
                            log_format = LOG_TEMPLATE_SQL;
                        } else if (strcasecmp(optarg, "csv") == 0) {
                            log_format = LOG_TEMPLATE_CSV;
                        } else if (strncasecmp(optarg, "template:", strlen("template:")) == 0) {
                            const char *template = optarg + strlen("template:");
                            if (!is_valid_log_template(template)) {
                                print_error("illegal value for --log-format: %s", optarg);
                                status = 1;
                                goto cleanup;
                            }
                            log_format = template;
                        } else {
                            print_error("illegal value for --log-format: %s", optarg);
                            status = 1;
                            goto cleanup;
                        }
                        break;

                    case OPT_START_RESTART:
                        if (strcasecmp("ALWAYS", optarg) == 0) {
                            restart = RESTART_ALWAYS;
                        } else if (strcasecmp("NEVER", optarg) == 0) {
                            restart = RESTART_NEVER;
                        } else if (strcasecmp("FAILURE", optarg) == 0) {
                            restart = RESTART_FAILURE;
                        } else {
                            print_error("illegal value for --restart: %s", optarg);
                            status = 1;
                            goto cleanup;
                        }
                        break;

                    case OPT_START_CHROOT:
                    {
                        if (!*optarg) {
                            print_error("--chroot cannot be empty string");
                            status = 1;
                            goto cleanup;
                        }

                        free(chroot_path);
                        chroot_path = abspath(optarg);
                        if (chroot_path == NULL) {
                            print_error("getting absolute path of --chroot=%s: %s", optarg, strerror(errno));
                            status = 1;
                            goto cleanup;
                        }
                        break;
                    }

                    case OPT_START_CRASH_REPORT:
                        crash_report = optarg;
                        break;

                    case OPT_START_RESTART_SLEEP:
                    {
                        char *endptr = NULL;
                        unsigned long value = strtoul(optarg, &endptr, 10);
                        if (!*optarg || *endptr || value > UINT_MAX) {
                            print_error("illegal value for --restart-sleep: %s", optarg);
                            status = 1;
                            goto cleanup;
                        }
                        restart_sleep = value;
                        break;
                    }

                    default:
                        assert(false);
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

            case 'N':
            {
                char *endptr = NULL;
                long value = strtol(optarg, &endptr, 10);
                if (!*optarg || *endptr || value > 19 || value < -20) {
                    print_error("illegal value for --priority: %s", optarg);
                    status = 1;
                    goto cleanup;
                }
                set_priority = true;
                priority     = value;
                break;
            }

            case 'k':
            {
                char *endptr = NULL;
                unsigned long value = strtoul(optarg, &endptr, 8);
                if (!*optarg || *endptr || value > 0777) {
                    print_error("illegal value for --umask: %s", optarg);
                    status = 1;
                    goto cleanup;
                }
                set_umask   = true;
                umask_value = value;
                break;
            }

            case 'r':
            {
                if (rlimits_count == rlimits_capacity) {
                    // Currently there are only 16 different rlimit values anyway.
                    if (SIZE_MAX - (RLIMITS_GROW * sizeof(struct rlimit_params)) < rlimits_capacity * sizeof(struct rlimit_params)) {
                        errno = ENOMEM;
                        print_error("cannot allocate memory for --rlimit: %s", optarg);
                        status = 1;
                        goto cleanup;
                    }
                    size_t new_capacity = rlimits_capacity + RLIMITS_GROW;
                    struct rlimit_params *new_rlimits = realloc(rlimits, new_capacity * sizeof(struct rlimit_params));
                    if (new_rlimits == NULL) {
                        print_error("cannot allocate memory for --rlimit: %s", optarg);
                        status = 1;
                        goto cleanup;
                    }

                    rlimits = new_rlimits;
                    rlimits_capacity = new_capacity;
                }

                if (!parse_rlimit_params(optarg, rlimits + rlimits_count)) {
                    print_error("illegal argument for --rlimit: %s", optarg);
                    status = 1;
                    goto cleanup;
                }

                if (rlimits[rlimits_count].resource == RLIMIT_FSIZE) {
                    // we need to pipe to the logfile here too!
                    rlimit_fsize = true;
                }

                ++ rlimits_count;
                break;
            }
            case 'C':
                if (!*optarg) {
                    print_error("--chdir cannot be empty string");
                    status = 1;
                    goto cleanup;
                }

                chdir_path = optarg;
                break;

            case '?':
                short_usage(argc, argv);
                status = 1;
                goto cleanup;

            default:
                assert(false);
        }
    }

    // because of skipped first argument:
    ++ optind;

    int count = argc - optind;
    if (count < 2) {
        print_error("not enough arguments");
        short_usage(argc, argv);
        status = 1;
        goto cleanup;
    }

    const char *name = argv[optind ++];
    const char *command = argv[optind];
    char **command_argv = argv + optind;

    if (!is_valid_name(name)) {
        print_error("illegal name: '%s'", name);
        short_usage(argc, argv);
        status = 1;
        goto cleanup;
    }

    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    if (user != NULL && get_uid_from_name(user, &uid) != 0) {
        print_error("getting user ID for %s: %s", user, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (group != NULL && get_gid_from_name(group, &gid) != 0) {
        print_error("getting group ID for %s: %s", group, strerror(errno));
        status = 1;
        goto cleanup;
    }

    uid_t selfuid = geteuid();
    uid_t selfgid = getegid();

    uid_t xuid = user  == NULL ? selfuid : uid;
    gid_t xgid = group == NULL ? selfgid : gid;

    // setup chroot, chdir, command path and check if it all can be accessed
    if (chroot_path != NULL) {
        // chroot case
        if (!can_list(chroot_path, xuid, xgid)) {
            print_error("illegal chroot path: %s: %s", chroot_path, strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (chdir_path != NULL) {
            if (chdir_path[0] != '/') {
                char *abs_chdir_path = join_path("/", chdir_path);
                if (abs_chdir_path == NULL) {
                    print_error("join_path(\"/\", \"%s\"): %s", chdir_path, strerror(errno));
                    status = 1;
                    goto cleanup;
                }
                chdir_path = abs_chdir_path;
                free_chdir_path = true;
            }

            char *norm_chdir_path = normpath_no_escape(chdir_path);
            if (norm_chdir_path == NULL) {
                print_error("normpath_no_escape(\"%s\"): %s", chdir_path, strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (free_chdir_path) {
                free((char*)chdir_path);
            }
            chdir_path = norm_chdir_path;
            free_chdir_path = true;

            char *chroot_chdir_path = join_path(chroot_path, chdir_path);
            if (chroot_chdir_path == NULL) {
                print_error("join_path(\"%s\", \"%s\"): %s", chroot_path, chdir_path, strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (!can_list(chroot_chdir_path, xuid, xgid)) {
                print_error("illegal chdir path: %s: %s", chroot_chdir_path, strerror(errno));
                free(chroot_chdir_path);
                status = 1;
                goto cleanup;
            }
            free(chroot_chdir_path);
        }

        if (command[0] != '/') {
            char *chroot_abs_command = join_path(chdir_path == NULL ? "/" : chdir_path, command);
            if (chroot_abs_command == NULL) {
                print_error("join_path(\"%s\", \"%s\"): %s", chdir_path == NULL ? "/" : chdir_path, command, strerror(errno));
                status = 1;
                goto cleanup;
            }

            char *norm_command = normpath_no_escape(chroot_abs_command);
            if (norm_command == NULL) {
                print_error("normpath_no_escape(\"%s\"): %s", chroot_abs_command, strerror(errno));
                free(chroot_abs_command);
                status = 1;
                goto cleanup;
            }
            free(chroot_abs_command);

            command = norm_command;
            free_command = true;
        } else {
            char *norm_command = normpath_no_escape(command);
            if (norm_command == NULL) {
                print_error("normpath_no_escape(\"%s\"): %s", command, strerror(errno));
                status = 1;
                goto cleanup;
            }

            command = norm_command;
            free_command = true;
        }

        char *abs_command = join_path(chroot_path, command);
        if (abs_command == NULL) {
            print_error("join_path(\"%s\", \"%s\"): %s", chroot_path, command, strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (!can_execute(abs_command, xuid, xgid)) {
            print_error("illegal service executable: %s: %s", abs_command, strerror(errno));
            free(abs_command);
            status = 1;
            goto cleanup;
        }
        free(abs_command);
    } else {
        // non-chroot case
        if (chdir_path != NULL) {
            char *abs_chdir_path = abspath(chdir_path);
            if (abs_chdir_path == NULL) {
                print_error("abspath(\"%s\"): %s", chdir_path, strerror(errno));
                status = 1;
                goto cleanup;
            }
            chdir_path = abs_chdir_path;
            free_chdir_path = true;

            if (!can_list(chdir_path, xuid, xgid)) {
                print_error("illegal chdir path: %s: %s", chdir_path, strerror(errno));
                status = 1;
                goto cleanup;
            }

            char *abs_command = join_path(abs_chdir_path, command);
            if (abs_command == NULL) {
                print_error("join_path(\"%s\", \"%s\"): %s", abs_chdir_path, command, strerror(errno));
                status = 1;
                goto cleanup;
            }

            command = abs_command;
            free_command = true;
        } else if (command[0] != '/') {
            char *abs_command = abspath(command);
            if (abs_command == NULL) {
                print_error("abspath(\"%s\"): %s", command, strerror(errno));
                status = 1;
                goto cleanup;
            }

            command = abs_command;
            free_command = true;
        }

        if (!can_execute(command, xuid, xgid)) {
            print_error("illegal service executable: %s: %s", command, strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    if (crash_report != NULL && !can_execute(crash_report, selfuid, selfgid)) {
        print_error("illegal crash report executable: %s: %s", crash_report, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (set_priority) {
        // XXX: I just do not understand the getrlimit() interface for RLIMIT_NICE.
        //      It always returns rlim.rlim_cur == 0, which is outside of the range
        //      of values defined in the man-page.

        // So instead I simply set the priority value already here and the
        // service-runner process will also run at the given priority.
        if (setpriority(PRIO_PROCESS, 0, priority) != 0) {
            print_error("cannot set process priority of service to %d: %s", priority, strerror(errno));
            status = 1;
            goto cleanup;
        }
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
            print_error("malloc(%zu): %s", pidfile_runner_size, strerror(errno));
            status = 1;
            goto cleanup;
        }

        int count = snprintf(pidfile_runner, pidfile_runner_size, "%s.runner", pidfile);
        assert(count >= 0 && (size_t)count == pidfile_runner_size - 1); (void)count;
    }

    if (!can_read_write(pidfile, selfuid, selfgid)) {
        print_error("cannot read and write file: %s: %s", pidfile, strerror(errno));
        status = 1;
        goto cleanup;
    }

    {
        // checking for existing process and trying to deal with it
        pid_t runner_pid = 0;
        if (read_pidfile(pidfile_runner, &runner_pid) == 0) {
            errno = 0;
            if (kill(runner_pid, 0) == 0 || errno == EPERM) {
                print_error("%s is already running", name);
                goto cleanup;
            } else if (errno == ESRCH) {
                print_error("%s exists, but PID %d doesn't exist.", pidfile_runner, runner_pid);
                pid_t other_pid = 0;
                if (read_pidfile(pidfile, &other_pid) == 0) {
                    if (kill(other_pid, 0) == 0) {
                        print_error("Service is running (PID: %d), but it's service-runner is not!", other_pid);
                        print_error("You probably will want to kill that process?");
                        status = 1;
                        goto cleanup;
                    }

                    if (unlink(pidfile) != 0 && errno != ENOENT) {
                        print_error("unlink(\"%s\"): %s", pidfile, strerror(errno));
                        status = 1;
                        goto cleanup;
                    }
                }

                if (unlink(pidfile_runner) != 0 && errno != ENOENT) {
                    print_error("unlink(\"%s\"): %s", pidfile_runner, strerror(errno));
                    status = 1;
                    goto cleanup;
                }
            } else {
                print_error("kill(%d, 0): %s", runner_pid, strerror(errno));
                status = 1;
                goto cleanup;
            }
        }
    }

    const bool do_logrotate = strchr(logfile, '%') != NULL;
    const bool do_pipe = do_logrotate || rlimit_fsize;
    const char *logfile_path;

    // TODO: validate log_format

    if (do_logrotate) {
        const time_t now = time(NULL);
        struct tm local_now;
        if (localtime_r(&now, &local_now) == NULL) {
            print_error("getting local time: %s", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (strftime(logfile_path_buf, sizeof(logfile_path_buf), logfile, &local_now) == 0) {
            print_error("cannot format logfile \"%s\": %s", logfile, strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (!can_read_write(logfile_path_buf, selfuid, selfgid)) {
            print_error("cannot read and write file: %s", logfile);
            status = 1;
            goto cleanup;
        }

        logfile_path = logfile_path_buf;
    } else {
        logfile_path = logfile;
    }

    logfile_fd = open(logfile_path, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
    if (logfile_fd == -1) {
        print_error("cannot open logfile: %s: %s", logfile_path, strerror(errno));
        status = 1;
        goto cleanup;
    }

    if (chown_logfile && fchown(logfile_fd, xuid, xgid) != 0) {
        print_error("cannot change owner of logfile: %s: %s", logfile_path, strerror(errno));
        status = 1;
        goto cleanup;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        print_error("fork for deamonize failed: %s", strerror(errno));
        status = 1;
        goto cleanup;
    } else if (pid != 0) {
        // parent: shell command quitting
        goto cleanup;
    }

    // child: service-runner process
    cleanup_pidfiles = true;
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
        sigaddset(&mask, SIGQUIT);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGCHLD);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
            print_error("sigprocmask(SIG_BLOCK, &mask, NULL): %s", strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    const pid_t runner_pid = getpid();
    {
        if (write_pidfile(pidfile_runner, runner_pid) != 0) {
            print_error("write_pidfile(\"%s\", %u): %s", pidfile_runner, getpid(), strerror(errno));
            status = 1;
            goto cleanup;
        }

        // setup standard I/O
        if (close(STDIN_FILENO) != 0 && errno != EBADFD) {
            print_error("close(STDIN_FILENO): %s", strerror(errno));
            status = 1;
            goto cleanup;
        }

        int stdin_fd = open("/dev/null", O_RDONLY);
        if (stdin_fd == -1) {
            print_error("open(\"/dev/null\", O_RDONLY): %s", strerror(errno));
            status = 1;
            goto cleanup;
        }

        if (stdin_fd != STDIN_FILENO) {
            if (dup2(stdin_fd, STDIN_FILENO) == -1) {
                print_error("dup2(stdin_fd, STDIN_FILENO): %s", strerror(errno));
                status = 1;
                goto cleanup;
            }

            if (close(stdin_fd) != 0) {
                print_error("close(stdin_fd): %s", strerror(errno));
                status = 1;
                goto cleanup;
            }
        }

        fflush(stdout);
        if (dup2(logfile_fd, STDOUT_FILENO) == -1) {
            print_error("dup2(logfile_fd, STDOUT_FILENO): %s", strerror(errno));
            status = 1;
            goto cleanup;
        }

        fflush(stderr);
        if (dup2(logfile_fd, STDERR_FILENO) == -1) {
            print_error("dup2(logfile_fd, STDERR_FILENO): %s", strerror(errno));
            status = 1;
            goto cleanup;
        }
    }

    running = true;
    while (running) {
        if (do_pipe) {
            pipefd[PIPE_READ ] = -1;
            pipefd[PIPE_WRITE] = -1;

            // logging pipe
            // if no log-rotating is done stdout/stderr pipes directly to the logfile, no need for the pipe
            int result = pipe(pipefd);
            if (result != 0) {
                print_error("pipe(pipefd): %s", strerror(errno));
                status = 1;
                goto cleanup;
            }

            int flags = fcntl(pipefd[PIPE_READ], F_GETFL, 0);
            if (flags == -1) {
                print_error("fcntl(pipefd[PIPE_READ], F_GETFL, 0): %s", strerror(errno));
                flags = 0;
            }

            if (fcntl(pipefd[PIPE_READ], F_SETFL, flags | O_NONBLOCK) == -1) {
                print_error("fcntl(pipefd[PIPE_READ], F_SETFL, flags | O_NONBLOCK): %s", strerror(errno));
            }
        }

        service_pid = fork();

        if (service_pid < 0) {
            print_error("fork for starting service failed: %s", strerror(errno));
            status = 1;
            goto cleanup;
        } else if (service_pid == 0) {
            // child: service process
            cleanup_pidfiles = false;

            if (write_pidfile(pidfile, getpid()) != 0) {
                print_error("(child) write_pidfile(\"%s\", %u): %s", pidfile, getpid(), strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            if (do_pipe) {
                if (close(pipefd[PIPE_READ]) != 0) {
                    print_error("(child) close(pipefd[PIPE_READ]): %s", strerror(errno));
                }
                pipefd[PIPE_READ] = -1;
            }

            if (close(logfile_fd) != 0) {
                print_error("(child) close(logfile_fd): %s", strerror(errno));
            } else {
                logfile_fd = -1;
            }

            // Because I don't know how to check if the target priority value is
            // allowed I have to already set it for the whole service-runner
            // process. (See above.)

            // Maybe I should do setrlimit() in the parent, too?
            // But a user would expect things like RLIMIT_FSIZE to only apply to the service and
            // not the service runner, i.e. the logfile shouldn't be limited by it.
            for (size_t index = 0; index < rlimits_count; ++ index) {
                struct rlimit_params *lim = &rlimits[index];
                if (setrlimit(lim->resource, &lim->limit) != 0) {
                    print_error("(child) setrlimit(%d, { .rlim_cur = %ld, .rlim_max = %ld }): %s",
                        lim->resource,
                        lim->limit.rlim_cur,
                        lim->limit.rlim_max,
                        strerror(errno));
                    signal_premature_exit(runner_pid);
                    status = 1;
                    goto cleanup;
                }
            }

            if (set_umask) {
                umask(umask_value);
            }

            if (do_pipe) {
                if (pipefd[PIPE_WRITE] != STDOUT_FILENO) {
                    fflush(stdout);
                    if (dup2(pipefd[PIPE_WRITE], STDOUT_FILENO) == -1) {
                        print_error("(child) dup2(pipefd[PIPE_WRITE], STDOUT_FILENO): %s", strerror(errno));
                        signal_premature_exit(runner_pid);
                        status = 1;
                        goto cleanup;
                    }
                }

                if (pipefd[PIPE_WRITE] != STDERR_FILENO) {
                    fflush(stderr);
                    if (dup2(pipefd[PIPE_WRITE], STDERR_FILENO) == -1) {
                        print_error("(child) dup2(pipefd[PIPE_WRITE], STDERR_FILENO): %s", strerror(errno));
                        signal_premature_exit(runner_pid);
                        status = 1;
                        goto cleanup;
                    }
                }

                if (pipefd[PIPE_WRITE] != STDOUT_FILENO && pipefd[PIPE_WRITE] != STDERR_FILENO && close(pipefd[PIPE_WRITE]) != 0) {
                    print_error("(child) close(pipefd[PIPE_WRITE]): %s", strerror(errno));
                    // though, ignore it anyway?
                }
                pipefd[PIPE_WRITE] = -1;
            }

            if (chroot_path != NULL && chroot(chroot_path) != 0) {
                print_error("(child) chroot(\"%s\"): %s", chroot_path, strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            if (chdir_path != NULL && chdir(chdir_path) != 0) {
                print_error("(child) chdir(\"%s\"): %s", chdir_path, strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            // signal masks are preserved across exec*()
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGTERM);
            sigaddset(&mask, SIGQUIT);
            sigaddset(&mask, SIGINT);
            sigaddset(&mask, SIGUSR1);
            sigaddset(&mask, SIGCHLD);
            if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                print_error("(child) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s", strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            // drop _supplementary_ group IDs
            if (group != NULL && setgroups(0, NULL) != 0) {
                print_error("(child) setgroups(0, NULL): %s", strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            if (group != NULL && setgid(gid) != 0) {
                print_error("(child) setgid(%u): %s", gid, strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            if (user != NULL && setuid(uid) != 0) {
                print_error("(child) setuid(%u): %s", uid, strerror(errno));
                signal_premature_exit(runner_pid);
                status = 1;
                goto cleanup;
            }

            // start service
            execv(command, command_argv);

            print_error("(child) execv(\"%s\", command_argv): %s", command, strerror(errno));
            signal_premature_exit(runner_pid);
            status = 1;
            goto cleanup;
        } else {
            // parent: service-runner process
            {
                // setup signal handling
                if (signal(SIGTERM, handles_stop_signal) == SIG_ERR) {
                    print_error("(parent) signal(SIGTERM, handles_stop_signal): %s", strerror(errno));
                }

                if (signal(SIGQUIT, handles_stop_signal) == SIG_ERR) {
                    print_error("(parent) signal(SIGQUIT, handles_stop_signal): %s", strerror(errno));
                }

                if (signal(SIGINT, handles_stop_signal) == SIG_ERR) {
                    print_error("(parent) signal(SIGINT, handles_stop_signal): %s", strerror(errno));
                }

                if (signal(SIGUSR1, handle_restart_signal) == SIG_ERR) {
                    print_error("(parent) signal(SIGUSR1, handle_restart_signal): %s", strerror(errno));
                }

                sigset_t mask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGTERM);
                sigaddset(&mask, SIGQUIT);
                sigaddset(&mask, SIGINT);
                sigaddset(&mask, SIGUSR1);
                if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                    print_error("(parent) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s", strerror(errno));
                }
            }

            if (do_pipe && close(pipefd[PIPE_WRITE]) != 0) {
                print_error("(parent) close(pipefd[PIPE_WRITE]): %s", strerror(errno));
                // though, ignore it anyway?
            }
            pipefd[PIPE_WRITE] = -1;

            service_pidfd = pidfd_open(service_pid, 0);
            if (service_pidfd == -1 && errno != ENOSYS) {
                print_error("(parent) pidfd_open(%u): %s", service_pid, strerror(errno));

                // wait a bit so the signal handlers are resetted again in child
                struct timespec wait_time = {
                    .tv_sec  = 0,
                    .tv_nsec = 500000000,
                };
                nanosleep(&wait_time, NULL);

                if (kill(service_pid, SIGTERM) != 0 && errno != ESRCH) {
                    print_error("(parent) kill(%u, SIGTERM): %s", service_pid, strerror(errno));
                }

                for (;;) {
                    int child_status = 0;
                    int result = waitpid(service_pid, &child_status, 0);
                    if (result < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno != ECHILD) {
                            print_error("(parent) waitpid(%u, &status, 0): %s", service_pid, strerror(errno));
                        }
                    } else if (result == 0) {
                        assert(false);
                        continue;
                    } else if (child_status != 0) {
                        if (WIFSIGNALED(child_status)) {
                            print_error("service PID %u exited with signal %d", service_pid, WTERMSIG(child_status));
                        } else {
                            print_error("service PID %u exited with status %d", service_pid, WEXITSTATUS(child_status));
                        }
                    }
                    break;
                }

                status = 1;
                goto cleanup;
            }

            // setup polling
            #define POLLFD_PID  0
            #define POLLFD_PIPE 1

            struct pollfd pollfds[] = {
                [POLLFD_PID ] = { service_pidfd,     POLLIN, 0 },
                [POLLFD_PIPE] = { pipefd[PIPE_READ], do_pipe ? POLLIN : 0, 0 },
            };

            if (service_pidfd == -1) {
                // for systems that don't support pidfd
                pollfds[POLLFD_PID ].revents = 0;
                if (signal(SIGCHLD, handle_child) == SIG_ERR) {
                    print_error("signal(SIGCHLD, handle_child): %s", strerror(errno));
                }
            }

            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                print_error("(parent) sigprocmask(SIG_UNBLOCK, &mask, NULL): %s", strerror(errno));
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
                        print_error("(parent) poll(): %s", strerror(errno));
                        break;
                    }
                }

                if (do_pipe) {
                    if (pollfds[POLLFD_PIPE].revents & POLLIN) {
                        // log-handling
                        if (do_logrotate) {
                            time_t now = time(NULL);
                            struct tm local_now;
                            char new_logfile_path_buf[PATH_MAX];

                            if (localtime_r(&now, &local_now) == NULL) {
                                print_error("(parent) getting local time: %s", strerror(errno));
                                status = 1;
                                goto cleanup;
                            }

                            if (strftime(new_logfile_path_buf, sizeof(new_logfile_path_buf), logfile, &local_now) == 0) {
                                print_error("(parent) cannot format logfile \"%s\": %s", logfile, strerror(errno));
                                status = 1;
                                goto cleanup;
                            }

                            if (strcmp(new_logfile_path_buf, logfile_path_buf) != 0) {
                                int new_logfile_fd = open(new_logfile_path_buf, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
                                if (logfile_fd == -1) {
                                    print_error("(parent) cannot open logfile: %s: %s", new_logfile_path_buf, strerror(errno));
                                }

                                if (chown_logfile && fchown(new_logfile_fd, xuid, xgid) != 0) {
                                    print_error("(parent) cannot change owner of logfile: %s: %s", new_logfile_path_buf, strerror(errno));
                                }

                                if (close(logfile_fd) != 0) {
                                    print_error("(parent) close(logfile_fd): %s", strerror(errno));
                                }

                                logfile_fd = new_logfile_fd;
                                fflush(stdout);
                                if (dup2(logfile_fd, STDOUT_FILENO) == -1) {
                                    print_error("(parent) dup2(logfile_fd, STDOUT_FILENO): %s", strerror(errno));
                                }

                                fflush(stderr);
                                if (dup2(logfile_fd, STDERR_FILENO) == -1) {
                                    print_error("(parent) dup2(logfile_fd, STDERR_FILENO): %s", strerror(errno));
                                }

                                strcpy(logfile_path_buf, new_logfile_path_buf);
                            }
                        }

                        // handle log messages
                        ssize_t count = splice(pipefd[PIPE_READ], NULL, logfile_fd, NULL, SPLICE_SZIE, SPLICE_F_NONBLOCK);
                        if (count < 0 && errno != EINTR) {
                            if (errno == EINVAL) {
                                // the docker volume filesystem doesn't support splice()
                                // and sendfile() doesn't support out_fd with O_APPEND set
                                // -> manual read()/write()
                                char buf[BUFSIZ];
                                ssize_t rcount = read(pipefd[PIPE_READ], buf, sizeof(buf));
                                if (rcount < 0) {
                                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                        print_error("(parent) read(pipefd[PIPE_READ], buf, sizeof(buf)): %s",
                                            strerror(errno));
                                    }
                                    break;
                                } else {
                                    size_t offset = 0;
                                    while (offset < rcount) {
                                        ssize_t wcount = write(logfile_fd, buf + offset, rcount - offset);
                                        if (wcount < 0) {
                                            if (errno == EINTR) {
                                                continue;
                                            }
                                            print_error("(parent) write(logfile_fd, buf + offset, rcount - offset): %s",
                                                strerror(errno));
                                            break;
                                        }

                                        offset += wcount;
                                    }
                                }
                            } else {
                                print_error("(parent) splice(pipefd[PIPE_READ], NULL, logfile_fd, NULL, SPLICE_SZIE, SPLICE_F_NONBLOCK): %s",
                                    strerror(errno));
                            }
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
                        print_error("(parent) waitpid(%d, &service_status, WNOHANG): %s", service_pid, strerror(errno));
                    } else {
                        // block signals again until we're ready again
                        // Should that be done before waitpid()?
                        // (and unblock again in the other two branches of this if-else-if-block)
                        sigset_t mask;
                        sigemptyset(&mask);
                        sigaddset(&mask, SIGTERM);
                        sigaddset(&mask, SIGQUIT);
                        sigaddset(&mask, SIGINT);
                        sigaddset(&mask, SIGUSR1);
                        sigaddset(&mask, SIGCHLD);
                        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
                            print_error("(parent) sigprocmask(SIG_BLOCK, &mask, NULL): %s", strerror(errno));
                        }

                        pollfds[POLLFD_PID].events = 0;
                        bool crash = false;
                        int param = 0;
                        const char *code_str = NULL;

                        if (WIFEXITED(service_status)) {
                            param = WEXITSTATUS(service_status);
                            code_str = "EXITED";

                            if (param == 0) {
                                print_info("%s exited normally", name);
                                if (!restart_issued && restart != RESTART_ALWAYS) {
                                    running = false;
                                }
                            } else {
                                print_error("%s exited with error status %d", name, param);
                                crash = true;
                                if (!restart_issued && restart == RESTART_NEVER) {
                                    running = false;
                                }
                            }
                        } else if (WIFSIGNALED(service_status)) {
                            param = WTERMSIG(service_status);
                            if (WCOREDUMP(service_status)) {
                                code_str = "DUMPED";
                                print_error("%s was killed by signal %d and dumped core", name, param);
                                crash = true;
                                if (!restart_issued && restart == RESTART_NEVER) {
                                    running = false;
                                }
                            } else {
                                code_str = "KILLED";
                                print_error("%s was killed by signal %d", name, param);

                                switch (param) {
                                    case SIGTERM:
                                        // We send SIGTERM for restart (no other signal),
                                        // so only do this in the SIGTERM case.
                                        if (restart_issued) {
                                            // don't set running to false
                                            break;
                                        }
                                    case SIGQUIT:
                                    case SIGINT:
                                    case SIGKILL:
                                        if (restart != RESTART_ALWAYS) {
                                            print_info("service stopped via signal %d -> don't restart", param);
                                            running = false;
                                        }
                                        break;

                                    default:
                                        crash = true;
                                        if (!restart_issued && restart == RESTART_NEVER) {
                                            running = false;
                                        }
                                        break;
                                }
                            }
                        } else {
                            assert(false);
                        }

                        restart_issued = false;
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
                                    print_error("(parent) clock_gettime(CLOCK_MONOTONIC, &ts_before): %s", strerror(errno));
                                }

                                char param_str[24];
                                int result = snprintf(param_str, sizeof(param_str), "%d", param);
                                assert(result > 0 && result < sizeof(param_str));

                                pid_t report_pid = 0;
                                result = posix_spawn(&report_pid, crash_report, NULL, NULL,
                                    (char*[]){ (char*)crash_report, (char*)name, (char*)code_str, param_str, (char*)logfile_path, NULL },
                                    environ);

                                if (result != 0) {
                                    print_error("(parent) starting crash reporter: %s", strerror(errno));
                                } else {
                                    for (;;) {
                                        int report_status = 0;
                                        int result = waitpid(report_pid, &report_status, 0);
                                        if (result < 0) {
                                            if (errno == EINTR) {
                                                continue;
                                            }
                                            print_error("(parent) waitpid(%u, &report_status, 0): %s", report_pid, strerror(errno));
                                        } else if (result == 0) {
                                            assert(false);
                                            continue;
                                        } else if (report_status != 0) {
                                            if (WIFSIGNALED(report_status)) {
                                                print_error("crash report PID %u exited with signal %d", report_pid, WTERMSIG(report_status));
                                            } else {
                                                print_error("crash report PID %u exited with status %d", report_pid, WEXITSTATUS(report_status));
                                            }
                                        }
                                        break;
                                    }
                                }

                                if (time_ok && clock_gettime(CLOCK_MONOTONIC, &ts_after) != 0) {
                                    time_ok = false;
                                    print_error("(parent) clock_gettime(CLOCK_MONOTONIC, &ts_after): %s", strerror(errno));
                                }
                            }

                            if (running && restart_sleep) {
                                if (time_ok) {
                                    time_t secs = ts_after.tv_sec - ts_before.tv_sec;
                                    if (secs <= restart_sleep) {
                                        sleep(restart_sleep - secs);
                                    }
                                } else {
                                    sleep(restart_sleep);
                                }
                            }
                        }
                    }
                }

                if (pollfds[POLLFD_PID].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    pollfds[POLLFD_PID].events = 0;
                }
            }

            if (do_pipe) {
                if (close(pipefd[PIPE_READ]) != 0) {
                    print_error("(parent) close(pipefd[PIPE_READ]): %s", strerror(errno));
                }
                pipefd[PIPE_READ] = -1;
            }

            if (service_pidfd != -1 && close(service_pidfd) != 0) {
                print_error("(parent) close(pidfd): %s", strerror(errno));
            }
            service_pidfd = -1;
        }

        if (running) {
            print_info("restarting %s...", name);
        }
    }

cleanup:
    if (cleanup_pidfiles) {
        if (unlink(pidfile) != 0 && errno != ENOENT) {
            print_error("unlink(\"%s\"): %s", pidfile, strerror(errno));
        }

        if (unlink(pidfile_runner) != 0 && errno != ENOENT) {
            print_error("unlink(\"%s\"): %s", pidfile_runner, strerror(errno));
        }
    }

    free(chroot_path);
    free(pidfile_runner);
    free(rlimits);

    if (free_chdir_path) {
        free((char*)chdir_path);
    }

    if (free_pidfile) {
        free((char*)pidfile);
    }

    if (free_logfile) {
        free((char*)logfile);
    }

    if (free_command) {
        free((char*)command);
    }

    if (pipefd[PIPE_READ] != -1) {
        close(pipefd[PIPE_READ]);
    }

    if (pipefd[PIPE_WRITE] != -1) {
        close(pipefd[PIPE_WRITE]);
    }

    if (logfile_fd != -1) {
        close(logfile_fd);
    }

    return status;
}
