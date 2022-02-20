#include <stdio.h>
#include <string.h>

#include "service_runner.h"

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
        "                                       Note that a second pidfile with the name FILE.runner is created\n"              \
        "                                       containing the process ID of the service-runner process itself.\n"              \
        "       --logfile=FILE, -l FILE         Write service output to FILE. default: /var/log/NAME-%%Y-%%m-%%d.log\n"         \
        "                                       This implements log-rotating based on the file name pattern.\n"                 \
        "                                       See `man strftime` for a description of the pattern language.\n"                \
        "       --user=USER, -u USER            Run service as USER (name or UID).\n"                                           \
        "       --group=GROUP, -g GROUP         Run service as GROUP (name or GID).\n"                                          \
        "       --crash-sleep=SECONDS           Wait SECONDS before restarting service. default: 1\n"                           \
        "       --crash-report=COMMAND          Run `COMMAND NAME CODE STATUS LOGFILE` if the service crashed.\n"               \
        "                                       CODE values:\n"                                                                 \
        "                                         EXITED ... service has exited, STATUS is it's exit status\n"                  \
        "                                         KILLED ... service was killed, STATUS is the killing signal\n"                \
        "                                         DUMPED ... service core dumped, STATUS is the killing signal\n"

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
