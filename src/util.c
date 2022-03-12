#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>

#include "service-runner.h"

static size_t dirend(const char *path) {
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
    if (path == NULL || !*path) {
        errno = EINVAL;
        return NULL;
    }

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
            assert(count >= 0 && (size_t)count == bufsize - 1); (void)count;
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
            assert(count >= 0 && (size_t)count == bufsize - 1); (void)count;
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

enum AbsPathResult get_pidfile_abspath(char **pidfile_ptr, const char *name) {
    char *pidfile = *pidfile_ptr;
    if (pidfile == NULL) {
        size_t bufsize = strlen("/var/run/.pid") + strlen(name) + 1;
        char *buf = malloc(bufsize);
        if (buf == NULL) {
            fprintf(stderr, "*** error: malloc(%zu): %s\n", bufsize, strerror(errno));
            return ABS_PATH_ERR;
        }

        int count = snprintf(buf, bufsize, "/var/run/%s.pid", name);
        assert(count >= 0 && (size_t)count == bufsize - 1); (void)count;

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

        int count = snprintf(buf, bufsize, "/var/log/%s-%%Y-%%m-%%d.log", name);
        assert(count >= 0 && (size_t)count == bufsize - 1); (void)count;

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

char *join_path(const char *dirpath, const char *relpath) {
    if (!*dirpath || !*relpath) {
        errno = EINVAL;
        return NULL;
    }

    size_t dirlen = strlen(dirpath);
    size_t rellen = strlen(relpath);
    size_t newsize = dirlen;
    
    if (SIZE_MAX - rellen < newsize) {
        errno = ENOMEM;
        return NULL;
    }
    newsize += rellen;

    if (SIZE_MAX - 1 < newsize) {
        errno = ENOMEM;
        return NULL;
    }
    ++ newsize;

    char *buf;
    if (dirpath[dirlen - 1] == '/' || relpath[0] == '/') {
        buf = malloc(newsize);
        if (buf == NULL) {
            return NULL;
        }
        memcpy(buf, dirpath, dirlen);
        memcpy(buf + dirlen, relpath, rellen);
    } else if (SIZE_MAX - 1 < newsize) {
        errno = ENOMEM;
        return NULL;
    } else {
        ++ newsize;
        buf = malloc(newsize);
        if (buf == NULL) {
            return NULL;
        }
        memcpy(buf, dirpath, dirlen);
        buf[dirlen] = '/';
        memcpy(buf + dirlen + 1, relpath, rellen);
    }

    buf[newsize - 1] = 0;
    return buf;
}

char *normpath_no_escape(const char *path) {
    if (!*path) {
        return strdup(".");
    }

    const size_t len = strlen(path) + 1;
    char *newpath = malloc(len);
    if (newpath == NULL) {
        return NULL;
    }

    const char *src = path;
    char *dest = newpath;

    while (*src) {
        if (src[0] == '.') {
            if (src[1] == '.') {
                if (src[2] == '/' || src[2] == 0) {
                    src += 2;
                    while (*src == '/') ++ src;
                    if (dest > newpath + 1) {
                        -- dest;
                        assert(*dest == '/');
                        while (dest > newpath && dest[-1] != '/') {
                            -- dest;
                        }
                        if (dest > newpath + 1 && dest[-1] == '/') {
                            -- dest;
                            if (src[-1] == '/') {
                                -- src;
                            }
                        }
                    }
                    continue;
                }
            } else if (src[1] == '/' || src[1] == 0) {
                src += 1;
                while (*src == '/') ++ src;
                if (dest > newpath + 1) {
                    -- dest;
                    assert(*dest == '/');
                    if (src[-1] == '/') {
                        -- src;
                    }
                }
                continue;
            }
        } else if (src[0] == '/') {
            ++ src;
            while (*src == '/') ++ src;
            if (dest > newpath && *src == 0) {
                continue;
            }
            *dest = '/';
            ++ dest;
            continue;
        }
        while (*src != '/' && *src != 0) {
            *dest = *src;
            ++ dest;
            ++ src;
        }
    }

    if (dest == newpath) {
        // This is safe because the case where path == "" and
        // thus the newly allocated space could only hold one byte
        // is handeled above.
        *dest = '.';
        ++ dest;
    }
    *dest = 0;
    ++ dest;

    const size_t newlen = dest - newpath;
    if (len != newlen) {
        char *shrunk = realloc(newpath, newlen);
        if (shrunk != NULL) {
            newpath = shrunk;
        }
    }

    return newpath;
}
