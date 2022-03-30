#ifndef SERVICE_RUNNER_H
#define SERVICE_RUNNER_H
#pragma once

#include <sys/types.h>
#include <sys/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVICE_RUNNER_VERSION_MAJOR 1
#define SERVICE_RUNNER_VERSION_MINOR 1
#define SERVICE_RUNNER_VERSION_PATCH 1

#ifdef NDEBUG
    #define LOG_TEMPLATE_TEXT "[%t] service-runner: [%L] %s"
    #define LOG_TEMPLATE_JSON "{\"level\":\"%l\",\"timestamp\":\"%T\",\"source\":\"service-runner\",\"message\":\"%js\"}"
    #define LOG_TEMPLATE_XML  "<log level=\"%l\" timestamp=\"%T\" source=\"service-runner\">%xs</log>"
    #define LOG_TEMPLATE_SQL  "INSERT INTO logs (level, timestamp, source, message) VALUES ('%l', '%T', 'service-runner', '%qs');"
    #define LOG_TEMPLATE_CSV_ "\"%l\",\"%T\",\"service-runner\",\"%cs\""
#else
    #define LOG_TEMPLATE_TEXT "[%t] service-runner: [%L] %f:%n: %s"
    #define LOG_TEMPLATE_JSON "{\"level\":\"%l\",\"timestamp\":\"%T\",\"source\":\"service-runner\",\"filename\":\"%jf\",\"lineno\":%n,\"message\":\"%js\"}"
    #define LOG_TEMPLATE_XML  "<log level=\"%l\" timestamp=\"%T\" source=\"service-runner\" filename=\"%xf\" lineno=\"%n\">%xs</log>"
    #define LOG_TEMPLATE_SQL  "INSERT INTO logs (level, timestamp, source, filename, lineno, message) VALUES ('%l', '%T', 'service-runner', '%qf', %n, '%qs');"
    #define LOG_TEMPLATE_CSV_ "\"%l\",\"%T\",\"service-runner\",\"%cs\",\"%cf\",%n"
#endif

#define LOG_TEMPLATE_CSV LOG_TEMPLATE_CSV_ "\r"
#define LOG_TEMPLATE_CSV_HELP LOG_TEMPLATE_CSV_ "\\r"

#ifdef __ILP32__
    #ifndef SYS_pidfd_open
        #define SYS_pidfd_open (__X32_SYSCALL_BIT + 434)
    #endif

    #ifndef SYS_pidfd_send_signal
        #define SYS_pidfd_send_signal (__X32_SYSCALL_BIT + 424)
    #endif
#else
    #ifndef SYS_pidfd_open
        #define SYS_pidfd_open 434
    #endif

    #ifndef SYS_pidfd_send_signal
        #define SYS_pidfd_send_signal 424
    #endif
#endif

#define pidfd_open(pid, flags) \
    syscall(SYS_pidfd_open, (pid), (flags))

#define pidfd_send_signal(pidfd, sig, info, flags) \
    syscall(SYS_pidfd_send_signal, (pidfd), (sig), (info), (flags))

char *abspath(const char *path);

void usage        (int argc, char *argv[]);
void short_usage  (int argc, char *argv[]);

int command_start    (int argc, char *argv[]);
int command_stop     (int argc, char *argv[]);
int command_restart  (int argc, char *argv[]);
int command_status   (int argc, char *argv[]);
int command_logrotate(int argc, char *argv[]);
int command_logs     (int argc, char *argv[]);
int command_help     (int argc, char *argv[]);

enum AbsPathResult {
    ABS_PATH_ERR,
    ABS_PATH_ORIG,
    ABS_PATH_NEW,
};

enum AbsPathResult get_pidfile_abspath(char **pidfile_ptr, const char *name);
enum AbsPathResult get_logfile_abspath(char **logfile_ptr, const char *name);

int write_pidfile(const char *pidfile, pid_t pid);
int read_pidfile(const char *pidfile, pid_t *pidptr);

char *join_pathv(const char *basepath, ...);
#define join_path(...) join_pathv(__VA_ARGS__, NULL)

char *normpath_no_escape(const char *path);

#ifdef __cplusplus
}
#endif

#endif // SERVICE_RUNNER_H
