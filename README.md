service-runner
==============

[![Test Status](https://img.shields.io/github/workflow/status/panzi/service-runner/Tests)](https://github.com/panzi/service-runner/actions/workflows/tests.yml)
[![License](https://img.shields.io/github/license/panzi/service-runner)](https://github.com/panzi/service-runner/blob/main/LICENSE)

My own service runner thing.

You might ask why? Surely there are enough such programs alread? Yeah, I felt
like it. Also, it is relatively minimal and does exactly what I want, which
includes:

* Start services
* Stop services
* Show service status
* Restarts the service on crash or manually
* Can report crashes
* Start service as a different user
* Start service in chroot environment and with certain rlimits and priority
* Logfile handling including log-rotate and setting owner of logfile
* Live display of logs

It uses `pidfd_open()` and `pidfd_send_signal()`, but has fallback code for when
that is not supported by the kernel. That makes the code a bit messy and redundant,
so maybe I remove the pidfd code again.

Usage
-----

```plain
Usage: service-runner start   <name> [options] [--] <command> [argument...]
       service-runner stop    <name> [options]
       service-runner restart <name> [options]
       service-runner status  <name> [options]
       service-runner logs    <name> [options]
       service-runner help [command]
       service-runner version

COMMANDS:

   service-runner start <name> [options] [--] <command> [argument...]

       Start <command> as service <name>. Does nothing if the service is already
       running. This automatically deamonizes, handles PID- and log-files, and 
       restarts on crash.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
                                       Note that a second pidfile with the name
                                       FILE.runner is created containing the 
                                       process ID of the service-runner process
                                       itself.
       -l, --logfile=FILE              Write service output to FILE. default: 
                                       /var/log/NAME-%%Y-%%m-%%d.log
                                       This implements log-rotating based on the
                                       file name pattern. See `man strftime` for
                                       a description of the pattern language.
           --chown-logfile             Change owner of the logfile to user/group
                                       specified by --user/--group.
           --restart=WHEN              Restart policy. Possible values for WHEN:
                                         NEVER ..... never restart (except when
                                                     explicitely requesting 
                                                     restart using the restart 
                                                     command)
                                         ALWAYS .... restart no matter if the 
                                                     service exited normally or
                                                     with an error status.
                                         FAILURE ... (default) only restart the
                                                     service if it exited with 
                                                     an error status or crashed.
       -u, --user=USER                 Run service as USER (name or UID).
       -g, --group=GROUP               Run service as GROUP (name or GID).
       -N, --priority=PRIORITY         Run service and service-runner(!) under 
                                       process scheduling priority PRIORITY. 
                                       From -20 (maximum priority) to +19 
                                       (minimum priority).
       -r, --rlimit=RES:SOFT[:HARD]    Run service with given resource limits. 
                                       This option can be defined multiple 
                                       times. SOFT/HARD may be an integer or 
                                       "INFINITY". RES may be an integer or one
                                       of these names: AS, CORE, CPU, DATA, 
                                       FSIZE, LOCKS, MEMLOCK, MSGQUEUE, NICE, 
                                       NOFILE, NPROC, RSS, RTPRIO, RTTIME, 
                                       SIGPENDING, STACK
                                       Note that it is not checked if calling 
                                       setrlimit() in the child process will 
                                       succeed before forking the child. This 
                                       means if it doesn't succeed there will be
                                       a crash-restart-loop.
                                       See: man setrlimit
       -k, --umask=UMASK               Run service with umask UMASK. Octal 
                                       values only.
       -C, --chdir=PATH                Change to directory PATH before running 
                                       the service. When --chroot is used chdir
                                       happens after chroot. The service binary
                                       path is relative to this PATH, even 
                                       without "./" prefix.
           --chroot=PATH               Call chroot with PATH before running the
                                       service (and before calling chdir, if 
                                       given). Unless --chdir is also given the
                                       service binary path is relative to this 
                                       PATH, even without "./" prefix.
           --crash-sleep=SECONDS       Wait SECONDS before restarting service. 
                                       default: 1
           --crash-report=COMMAND      Run `COMMAND NAME CODE STATUS LOGFILE` if
                                       the service crashed.
                                       CODE values:
                                         EXITED ... service has exited, STATUS 
                                                    is it's exit status
                                         KILLED ... service was killed, STATUS 
                                                    is the killing signal
                                         DUMPED ... service core dumped, STATUS
                                                    is the killing signal

   service-runner stop <name> [options]

       Stop service <name>. If --pidfile was passed to the corresponding start 
       command it must be passed with the same argument here again.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
           --shutdown-timeout=SECONDS  If the service doesn't shut down after 
                                       SECONDS after sending SIGTERM send 
                                       SIGKILL. -1 means no timeout, just wait 
                                       forever. default: -1

   service-runner restart <name> [options]

       Restart service <name>. Error if it's not already running.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid

   service-runner status <name> [options]

       Print some status information about service <name>.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid

   service-runner logs <name> [options]

       Print logs of service <name>.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
       -f, --follow                    Output new logs as they are written.

   service-runner help [command]

       Print help message to <command>. If no command is passed, prints help 
       message to all commands.

   service-runner version

       Print version string.

(c) 2022 Mathias Panzenböck
GitHub: https://github.com/panzi/service-runner
```
