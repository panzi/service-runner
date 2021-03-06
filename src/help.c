#include <sys/ioctl.h>
#include <sys/resource.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "service-runner.h"

#define WS_COL_DEFAULT 80
#define WS_ROW_DEFAULT 80
#define WS_XPIXEL_DEFAULT (80 *  8)
#define WS_YPIXEL_DEFAULT (40 * 14)

#ifdef RLIMIT_NICE
    #define RLIMIT_NICE_STR ", NICE"
#else
    #define RLIMIT_NICE_STR
#endif

#ifdef RLIMIT_RTPRIO
    #define RLIMIT_RTPRIO_STR ", RTPRIO"
#else
    #define RLIMIT_RTPRIO_STR
#endif

#ifdef RLIMIT_RTTIME
    #define RLIMIT_RTTIME_STR ", RTTIME"
#else
    #define RLIMIT_RTTIME_STR
#endif

#ifdef RLIMIT_SIGPENDING
    #define RLIMIT_SIGPENDING_STR ", SIGPENDING"
#else
    #define RLIMIT_SIGPENDING_STR
#endif

#ifdef RLIMIT_STACK
    #define RLIMIT_STACK_STR ", STACK"
#else
    #define RLIMIT_STACK_STR
#endif

#define HELP_OPT_PIDFILE \
        "       -p, --pidfile=FILE              Use FILE as the pidfile. default: /var/run/NAME.pid\n"

#define HELP_CMD_START_HDR                                                                                                      \
        "   %s start <name> [options] [--] <command> [argument...]\n"
#define HELP_CMD_START_DESCR \
        "\n"                                                                                                                    \
        "       Start <command> as service <name>. Does nothing if the service is already running. This automatically deamonizes, handles PID- and log-files, and restarts on crash.\n" \
        "\n"                                                                                                                    \
        "   OPTIONS:\n"                                                                                                         \
        HELP_OPT_PIDFILE                                                                                                        \
        "                                       Note that a second pidfile with the name FILE.runner is created containing the process ID of the service-runner process itself.\n" \
        "       -l, --logfile=FILE              Write service output to FILE. default: /var/log/NAME-%Y-%m-%d.log\n"            \
        "                                       This implements log-rotating based on the file name pattern. See `man strftime` for a description of the pattern language.\n" \
        "           --chown-logfile             Change owner of the logfile to user/group specified by --user/--group.\n"       \
        "           --log-format=FORMAT\n"                                                                                      \
        "\n"                                                                                                                    \
        "             Format of service-runner's own log messages.\n"                                                           \
        "             FORMAT values:\n"                                                                                         \
        "               text ................ '" LOG_TEMPLATE_TEXT "' (default)\n"                                              \
        "               json ................ '" LOG_TEMPLATE_JSON "'\n"                                                        \
        "               xml ................. '" LOG_TEMPLATE_XML "'\n"                                                         \
        "               sql ................. \"" LOG_TEMPLATE_SQL "\"\n"                                                       \
        "               csv ................. '" LOG_TEMPLATE_CSV_HELP "'\n"                                                    \
        "               template:TEMPLATE ... Interpolate given TEMPLATE.\n"                                                    \
        "\n"                                                                                                                    \
        "             Template syntax:\n"                                                                                       \
        "               %Y .... local time 4 digit year\n"                                                                      \
        "               %m .... local time 2 digit month\n"                                                                     \
        "               %d .... local time 2 digit day in month\n"                                                              \
        "               %H .... local time 2 digit hour (24 hour clock)\n"                                                      \
        "               %M .... local time 2 digit minute\n"                                                                    \
        "               %S .... local time 2 digit second\n"                                                                    \
        "               %a .... local time abbreviated day in the week name\n"                                                  \
        "               %b .... local time abbreviated month name\n"                                                            \
        "               %z .... local time zone offset\n"                                                                       \
        "               %gY ... GMT 4 digit year\n"                                                                             \
        "               %gm ... GMT 2 digit month\n"                                                                            \
        "               %gd ... GMT 2 digit day in month\n"                                                                     \
        "               %gH ... GMT 2 digit hour (24 hour clock)\n"                                                             \
        "               %gM ... GMT 2 digit minute\n"                                                                           \
        "               %gS ... GMT 2 digit second\n"                                                                           \
        "               %ga ... GMT abbreviated day in the week name\n"                                                         \
        "               %gb ... GMT abbreviated month name\n"                                                                   \
        "               %s .... log message\n"                                                                                  \
        "               %js ... JSON encoded log message (no enclosing quotes)\n"                                               \
        "               %xs ... XML encoded log message\n"                                                                      \
        "               %qs ... SQL encoded log message (no enclosing quotes)\n"                                                \
        "               %cs ... CSV encoded log message (no enclosing quotes)\n"                                                \
        "               %f .... source filename\n"                                                                              \
        "               %jf ... JSON encoded filename (no enclosing quotes)\n"                                                  \
        "               %xf ... XML encoded filename\n"                                                                         \
        "               %qf ... SQL encoded filename (no enclosing quotes)\n"                                                   \
        "               %cf ... CSV encoded filename (no enclosing quotes)\n"                                                   \
        "               %n .... line number\n"                                                                                  \
        "               %l .... \"info\" or \"error\"\n"                                                                        \
        "               %L .... \"INFO\" or \"ERROR\"\n"                                                                        \
        "               %t .... equivalent to '%Y-%m-%d %H:%M:%S%z'\n"                                                          \
        "               %T .... equivalent to '%Y-%m-%dT%H:%M:%S%z'\n"                                                          \
        "               %gt ... equivalent to '%gY-%gm-%gd %gH:%gM:%gSZ'\n"                                                                   \
        "               %gT ... equivalent to '%gY-%gm-%gdT%gH:%gM:%gSZ'\n"                                                                   \
        "               %h .... RFC 7231 IMF-fixdate: '%ga, %gd %gb %gY %gH:%gM:%gS GMT'\n"                                     \
        "               %% .... outputs %\n"                                                                                    \
        "\n"                                                                                                                    \
        "           --manual-logrotate          Pass this to enable manual log-rotation via the logrotate service-runner command.\n" \
        "           --restart=WHEN\n"                                                                                           \
        "\n"                                                                                                                    \
        "             Restart policy. Possible values for WHEN:\n"                                                              \
        "               NEVER ..... never restart (except when explicitely requesting restart using the restart command)\n"     \
        "               ALWAYS .... restart no matter if the service exited normally or with an error status.\n"                \
        "               FAILURE ... (default) only restart the service if it exited with an error status or crashed.\n"         \
        "\n"                                                                                                                    \
        "       -u, --user=USER                 Run service as USER (name or UID).\n"                                           \
        "       -g, --group=GROUP               Run service as GROUP (name or GID).\n"                                          \
        "       -N, --priority=PRIORITY         Run service and service-runner(!) under process scheduling priority PRIORITY. From -20 (maximum priority) to +19 (minimum priority).\n" \
        "       -r, --rlimit=RES:SOFT[:HARD]\n"                                                                                 \
        "\n"                                                                                                                    \
        "             Run service with given resource limits. This option can be defined multiple times. SOFT/HARD may be an integer or \"INFINITY\". RES may be an integer or one of these names: " \
                      "AS, CORE, CPU, DATA, FSIZE, LOCKS, MEMLOCK, MSGQUEUE" RLIMIT_NICE_STR                                    \
                      ", NOFILE, NPROC, RSS" RLIMIT_RTPRIO_STR RLIMIT_RTTIME_STR                                                \
                      RLIMIT_SIGPENDING_STR RLIMIT_STACK_STR "\n"                                                               \
        "\n"                                                                                                                    \
        "             Note that it is not checked if calling setrlimit() in the child process will succeed before forking the child. This means if it doesn't succeed there will be a crash-restart-loop.\n" \
        "             See: man setrlimit\n"                                                                                     \
        "\n"                                                                                                                    \
        "       -k, --umask=UMASK               Run service with umask UMASK. Octal values only.\n"                             \
        "       -C, --chdir=PATH                Change to directory PATH before running the service. When --chroot is used chdir happens after chroot. The service binary path is relative to this PATH, even without \"./\" prefix.\n" \
        "           --chroot=PATH               Call chroot with PATH before running the service (and before calling chdir, if given). Unless --chdir is also given the service binary path is relative to this PATH, even without \"./\" prefix.\n" \
        "           --restart-sleep=SECONDS     Wait SECONDS before restarting service. default: 1\n"                           \
        "           --crash-report=COMMAND\n"                                                                                   \
        "       -f, --foreground                Don't daemonize, but keep running in foreground.\n"                             \
        "\n"                                                                                                                    \
        "             Run `COMMAND NAME CODE STATUS LOGFILE` if the service crashed.\n"                                         \
        "             CODE values:\n"                                                                                           \
        "               EXITED ... service has exited, STATUS is it's exit status\n"                                            \
        "               KILLED ... service was killed, STATUS is the killing signal\n"                                          \
        "               DUMPED ... service core dumped, STATUS is the killing signal\n"

#define HELP_CMD_STOP_HDR                                                                                           \
        "   %s stop <name> [options]\n"
#define HELP_CMD_STOP_DESCR                                                                                         \
        "\n"                                                                                                        \
        "       Stop service <name>. If --pidfile was passed to the corresponding start command it must be passed with the same argument here again.\n" \
        "\n"                                                                                                        \
        "   OPTIONS:\n"                                                                                             \
        HELP_OPT_PIDFILE                                                                                            \
        "           --shutdown-timeout=SECONDS  If the service doesn't shut down after SECONDS after sending SIGTERM send SIGKILL. -1 means no timeout, just wait forever. default: -1\n"

#define HELP_CMD_RESTART_HDR                                                    \
        "   %s restart <name> [options]\n"
#define HELP_CMD_RESTART_DESCR                                                  \
        "\n"                                                                    \
        "       Restart service <name>. Error if it's not already running.\n"   \
        "\n"                                                                    \
        "   OPTIONS:\n"                                                         \
        HELP_OPT_PIDFILE

#define HELP_CMD_STATUS_HDR                                             \
        "   %s status <name> [options]\n"
#define HELP_CMD_STATUS_DESCR                                           \
        "\n"                                                            \
        "       Print some status information about service <name>.\n"  \
        "\n"                                                            \
        "   OPTIONS:\n"                                                 \
        HELP_OPT_PIDFILE

#define HELP_CMD_LOGROTATE_HDR                                                  \
        "   %s logrotate <name> [options]\n"
#define HELP_CMD_LOGROTATE_DESCR                                                \
        "\n"                                                                    \
        "       Issue manual log-rotate to service-runner process. The service-runner has to be started with the --manual-logrotate argument for this command to have any effect.\n" \
        "\n"                                                                    \
        "   OPTIONS:\n"                                                         \
        HELP_OPT_PIDFILE

#define HELP_CMD_LOGS_HDR                                                                \
        "   %s logs <name> [options]\n"
#define HELP_CMD_LOGS_DESCR                                                              \
        "\n"                                                                             \
        "       Print logs of service <name>.\n"                                         \
        "\n"                                                                             \
        "   OPTIONS:\n"                                                                  \
        HELP_OPT_PIDFILE \
        "       -f, --follow                    Output new logs as they are written.\n"

#define HELP_CMD_HELP_HDR           \
        "   %s help [command]\n"
#define HELP_CMD_HELP_DESCR         \
        "\n"                        \
        "       Print help message to <command>. If no command is passed, prints help message to all commands.\n"

#define HELP_CMD_VERSION_HDR        \
        "   %s version\n"
#define HELP_CMD_VERSION_DESCR      \
        "\n"                        \
        "       Print version string.\n"

const char *get_progname(int argc, char *argv[]) {
    return argc > 0 ? argv[0] : "service-runner";
}

// only can handle valid UTF-8
#define DECODE_UTF8(STR, SIZE, ...) \
    for (size_t index = 0; index < (SIZE);) { \
        uint8_t byte1 = (STR)[index ++]; \
        if (byte1 < 0x80) { \
            uint32_t codepoint = byte1; \
            (void)codepoint; \
            __VA_ARGS__; \
        } else if (byte1 < 0xE0) { \
            if (index < (SIZE)) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint32_t codepoint = (uint32_t)(byte1 & 0x1F) << 6 | \
                                     (uint32_t)(byte2 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } else if (byte1 < 0xF0) { \
            if (index + 1 < (SIZE)) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint8_t byte3 = (STR)[index ++]; \
                \
                uint32_t codepoint = (uint32_t)(byte1 & 0x0F) << 12 | \
                                     (uint32_t)(byte2 & 0x3F) <<  6 | \
                                     (uint32_t)(byte3 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } else if (byte1 < 0xF8) { \
            if (index + 2 < (SIZE)) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint8_t byte3 = (STR)[index ++]; \
                uint8_t byte4 = (STR)[index ++]; \
                \
                uint32_t codepoint = (uint32_t)(byte1 & 0x07) << 18 | \
                                     (uint32_t)(byte2 & 0x3F) << 12 | \
                                     (uint32_t)(byte3 & 0x3F) <<  6 | \
                                     (uint32_t)(byte4 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } /* else illegal byte sequence, ignored */ \
    }

#define DECODE_UTF8Z(STR, ...) \
    size_t index = 0; \
    while ((STR)[index]) { \
        size_t codepoint_index = index; \
        (void)codepoint_index; \
        uint8_t byte1 = (STR)[index ++]; \
        if (byte1 < 0x80) { \
            uint32_t codepoint = byte1; \
            (void)codepoint; \
            __VA_ARGS__; \
        } else if (byte1 < 0xE0) { \
            if ((STR)[index]) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint32_t codepoint = (uint32_t)(byte1 & 0x1F) << 6 | \
                                     (uint32_t)(byte2 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } else if (byte1 < 0xF0) { \
            if ((STR)[index] && (STR)[index + 1]) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint8_t byte3 = (STR)[index ++]; \
                \
                uint32_t codepoint = (uint32_t)(byte1 & 0x0F) << 12 | \
                                     (uint32_t)(byte2 & 0x3F) <<  6 | \
                                     (uint32_t)(byte3 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } else if (byte1 < 0xF8) { \
            if ((STR)[index] && (STR)[index + 1] && (STR)[index + 2]) { \
                uint8_t byte2 = (STR)[index ++]; \
                uint8_t byte3 = (STR)[index ++]; \
                uint8_t byte4 = (STR)[index ++]; \
                \
                uint32_t codepoint = (uint32_t)(byte1 & 0x07) << 18 | \
                                     (uint32_t)(byte2 & 0x3F) << 12 | \
                                     (uint32_t)(byte3 & 0x3F) <<  6 | \
                                     (uint32_t)(byte4 & 0x3F); \
                (void)codepoint; \
                __VA_ARGS__; \
            } /* else unexpected end of multibyte sequence, ignored */ \
        } /* else illegal byte sequence, ignored */ \
    }

bool is_breaking_space(uint32_t codepoint) {
    return codepoint == ' ' ||
           (codepoint >= 0x09 && codepoint <= 0x0D) ||
           codepoint == 0x2028 ||
           codepoint == 0x2029 ||
           (codepoint >= 0x2000 && codepoint <= 0x200A) ||
           codepoint == 0x205F ||
           codepoint == 0x3000;
}

size_t find_word_end(const char *text, size_t text_index) {
    const char *str = text + text_index;
    DECODE_UTF8Z(str, {
        if (is_breaking_space(codepoint)) {
            return text_index + codepoint_index;
        }
    });

    return text_index + index;
}

size_t find_word_start(const char *text, size_t text_index) {
    const char *str = text + text_index;
    DECODE_UTF8Z(str, {
        if (!is_breaking_space(codepoint) || codepoint == '\n') {
            return text_index + codepoint_index;
        }
    });

    return text_index + index;
}

// just something, probably not very correct
size_t count_graphemes(const char *text, size_t len) {
    size_t count = 0;
    DECODE_UTF8(text, len, {
        if (codepoint == '\t') {
            count += 8; // XXX: not how tabs work
        } else if (codepoint >= ' ' && codepoint <= '~') {
            // ASCII fast path
            count += 1;
        } else if (
            // control characters
            (codepoint >= 0x00 && codepoint <= 0x19) ||
            (codepoint >= 0x1A && codepoint <= 0x1F) ||
            (codepoint >= 0x7F && codepoint <= 0x9F) ||

            // zero width-space
            codepoint == 0x200B ||

            // Combining Diacritical Marks for Symbols
            (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||

            // Combining Half Marks
            (codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||

            // hebrew diacritics
            (codepoint >= 0x0591 && codepoint <= 0x05C7) ||

            // arabic diacritics
            (codepoint >= 0x0610 && codepoint <= 0x06ED) ||

            // TODO: many more here: https://www.compart.com/en/unicode/category/Mn

            // combining diacritic marks
            (codepoint >= 0x0300 && codepoint <= 0x036F) ||

            // line separator, paragraph separator
            (codepoint >= 0x2028 && codepoint <= 0x2029) ||

            // Combining Diacritical Marks Extended
            (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||

            // Combining Diacritical Marks Supplement
            (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||

            // Combining Half Marks
            (codepoint >= 0xFE20 && codepoint <= 0xFE2F)
        ) {
            // zero
        } else {
            count += 1;
        }
    });
    // fprintf(stderr, "%3zu graphemes in \"", count);
    // fwrite(text, len, 1, stderr);
    // fprintf(stderr, "\"\n");
    return count;
}

bool is_all_dots(const char *text, size_t len) {
    if (len < 3) {
        return false;
    }

    for (size_t index = 0; index < len; ++ index) {
        if (text[index] != '.') {
            return false;
        }
    }

    return true;
}

const char SPACES[256] =
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                ";

void print_spaces(FILE *fp, size_t count) {
    while (count >= sizeof(SPACES)) {
        fwrite(SPACES, sizeof(SPACES), 1, fp);
        count -= sizeof(SPACES);
    }
    fwrite(SPACES, count, 1, fp);
}

/**
 * @brief Prints a formatted plain text, wrapping lines in a way to keep some of the formatting.
 * 
 * It indents wrapped lines to:
 * - Existing indentation of non-breaking spaces.
 *   If the existing indentation alone is longer than linelen it is set to zero.
 * - Indentation marks consisting of two spaces '  ' (two U+0020) before non-space.
 * - Definition list items marked by '<term> ... <definition>' or more dots indent
 *   to the start of <definition>.
 * - List items marked by '- <text>' or '* <text>' (a single U+0020 space after '-' or '*')
 *   indent to the start of <text>.
 * 
 * @param fp is the stream to print to
 * @param text is the text to print
 * @param linelen is the maximum length of a line in graphemes
 */
void print_wrapped_text(FILE *fp, const char *text, size_t linelen) {
    size_t index = find_word_start(text, 0);
    size_t indent = count_graphemes(text, index);
    bool wrapped = false;
    bool was_all_dots = false;
    // fprintf(stderr, "new indent width (indent): %zu\n", indent);

    if (indent >= linelen) {
        indent = 0;
    } else {
        fwrite(text, index, 1, fp);
    }
    size_t current_linelen = indent;

    for (;;) {
        char ch = text[index];
        if (!ch) {
            break;
        }

        if (ch == '\n') {
            fputc('\n', fp);
            ++ index;
            size_t next_index = find_word_start(text, index);
            indent = count_graphemes(text + index, next_index - index);
            if (indent >= linelen) {
                indent = 0;
            } else {
                fwrite(text + index, next_index - index, 1, fp);
            }
            // fprintf(stderr, "new indent width (indent): %zu\n", indent);
            current_linelen = indent;
            index = next_index;
            wrapped = false;
            was_all_dots = false;
        } else {
            size_t word_end = find_word_end(text, index);
            size_t word_graphemes = count_graphemes(text + index, word_end - index);
            if (current_linelen + word_graphemes > linelen && current_linelen > indent) {
                fputc('\n', fp);
                // fprintf(stderr, "do indent by %zu spaces\n", indent);
                print_spaces(fp, indent);
                current_linelen = indent;
                wrapped = true;
            }

            fwrite(text + index, word_end - index, 1, fp);

            if (!wrapped) {
                char ch;
                if (current_linelen == indent && ((ch = text[index]) == '-' || ch == '*' || ch == '+') && (word_end - index) == 1) {
                    indent = current_linelen + 2;
                    // fprintf(stderr, "new indent width (list): %zu\n", indent);
                } else if (
                    (index >= 2 && ((ch = text[index - 2]) == ' ' || ch == '\t') && text[index - 1] == ' ') ||
                    (index >= 1 && text[index - 1] == '\t')
                ) {
                    // indentation mark
                    indent = current_linelen;
                    // fprintf(stderr, "new indent width (mark): %zu\n", indent);
                } else if (was_all_dots) {
                    // definition list item
                    indent = current_linelen;
                    // fprintf(stderr, "new indent width (def): %zu\n", indent);
                }

                was_all_dots = is_all_dots(text + index, word_end - index);
            }
            current_linelen += word_graphemes;

            index = find_word_start(text, word_end);

            size_t space_graphemes = count_graphemes(text + word_end, index - word_end);
            if (current_linelen + space_graphemes >= linelen) {
                char ch = text[index];
                if (ch != '\n' && ch != 0) {
                    fputc('\n', fp);
                    // fprintf(stderr, "do indent by %zu spaces\n", indent);
                    print_spaces(fp, indent);
                    current_linelen = indent;
                    wrapped = true;
                }
            } else {
                fwrite(text + word_end, index - word_end, 1, fp);
                current_linelen += space_graphemes;
            }
        }
    }
}

void short_usage(int argc, char *argv[]) {
    const char *progname = get_progname(argc, argv);
    printf("\n");
    printf("Usage: %s start     <name> [options] [--] <command> [argument...]\n", progname);
    printf("       %s stop      <name> [options]\n", progname);
    printf("       %s restart   <name> [options]\n", progname);
    printf("       %s status    <name> [options]\n", progname);
    printf("       %s logrotate <name> [options]\n", progname);
    printf("       %s logs      <name> [options]\n", progname);
    printf("       %s help [command]\n", progname);
    printf("       %s version\n", progname);
}

void usage(int argc, char *argv[]) {
    short_usage(argc, argv);
    
    const char *progname = get_progname(argc, argv);
    struct winsize wsize = {
        .ws_col = WS_COL_DEFAULT,
        .ws_row = WS_ROW_DEFAULT,
        .ws_xpixel = WS_XPIXEL_DEFAULT,
        .ws_ypixel = WS_YPIXEL_DEFAULT,
    };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize);

    print_wrapped_text(
        stdout,
        "\n"
        "COMMANDS:\n"
        "\n", wsize.ws_col);

    printf(HELP_CMD_START_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_START_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_STOP_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_STOP_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_RESTART_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_RESTART_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_STATUS_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_STATUS_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_LOGROTATE_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_LOGROTATE_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_LOGS_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_LOGS_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_HELP_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_HELP_DESCR "\n", wsize.ws_col);

    printf(HELP_CMD_VERSION_HDR, progname);
    print_wrapped_text(stdout, HELP_CMD_VERSION_DESCR "\n", wsize.ws_col);

    print_wrapped_text(
        stdout,
        "(c) 2022 Mathias Panzenb??ck\n"
        "GitHub: https://github.com/panzi/service-runner\n",
        wsize.ws_col
    );
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
    const char *progname = get_progname(argc, argv);
    struct winsize wsize = {
        .ws_col = WS_COL_DEFAULT,
        .ws_row = WS_ROW_DEFAULT,
        .ws_xpixel = WS_XPIXEL_DEFAULT,
        .ws_ypixel = WS_YPIXEL_DEFAULT,
    };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize);

    if (strcmp(command, "start") == 0) {
        printf("\n" HELP_CMD_START_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_START_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "stop") == 0) {
        printf("\n" HELP_CMD_STOP_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_STOP_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "restart") == 0) {
        printf("\n" HELP_CMD_RESTART_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_RESTART_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "status") == 0) {
        printf("\n" HELP_CMD_STATUS_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_STATUS_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "logrotate") == 0) {
        printf("\n" HELP_CMD_LOGROTATE_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_LOGROTATE_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "logs") == 0) {
        printf("\n" HELP_CMD_LOGS_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_LOGS_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "help") == 0) {
        printf("\n" HELP_CMD_HELP_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_HELP_DESCR, wsize.ws_col);
        return 0;
    } else if (strcmp(command, "version") == 0) {
        printf("\n" HELP_CMD_VERSION_HDR, progname);
        print_wrapped_text(stdout, HELP_CMD_VERSION_DESCR, wsize.ws_col);
        return 0;
    } else {
        fprintf(stderr, "*** error: illegal command: %s\n", command);
        short_usage(argc, argv);
        return 1;
    }
}
