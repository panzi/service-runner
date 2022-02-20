#ifndef SERVICE_RUNNER_H
#define SERVICE_RUNNER_H
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

char *abspath(const char *path);

void usage        (int argc, char *argv[]);
void short_usage  (int argc, char *argv[]);

int command_start (int argc, char *argv[]);
int command_stop  (int argc, char *argv[]);
int command_status(int argc, char *argv[]);
int command_help  (int argc, char *argv[]);

enum AbsPathResult {
    ABS_PATH_ERR,
    ABS_PATH_ORIG,
    ABS_PATH_NEW,
};

enum AbsPathResult get_pidfile_abspath(char **pidfile_ptr, const char *name);
enum AbsPathResult get_logfile_abspath(char **logfile_ptr, const char *name);

int write_pidfile(const char *pidfile, pid_t pid);
int read_pidfile(const char *pidfile, pid_t *pidptr);

#ifdef __cplusplus
}
#endif

#endif // SERVICE_RUNNER_H
