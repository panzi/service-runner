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
Usage: service-runner start     <name> [options] [--] <command> [argument...]
       service-runner stop      <name> [options]
       service-runner restart   <name> [options]
       service-runner status    <name> [options]
       service-runner logrotate <name> [options]
       service-runner logs      <name> [options]
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
                                       FILE.runner is created containing the process
                                       ID of the service-runner process itself.
       -l, --logfile=FILE              Write service output to FILE. default: 
                                       /var/log/NAME-%Y-%m-%d.log
                                       This implements log-rotating based on the file
                                       name pattern. See `man strftime` for a 
                                       description of the pattern language.
           --chown-logfile             Change owner of the logfile to user/group 
                                       specified by --user/--group.
           --log-format=FORMAT

             Format of service-runner's own log messages.
             FORMAT values:
               text ................ '[%t] service-runner: [%L] %s' (default)
               json ................ '{"level":"%l","timestamp":"%T","source":"service-runner","message":"%js"}'
               xml ................. '<log level="%l" timestamp="%T" source="service-runner">%xs</log>'
               sql ................. "INSERT INTO logs (level, timestamp, source, message) VALUES ('%l', '%T', 'service-runner', '%qs');"
               csv ................. '"%l","%T","service-runner","%cs"\r'
               template:TEMPLATE ... Interpolate given TEMPLATE.

             Template syntax:
               %Y .... local time 4 digit year
               %m .... local time 2 digit month
               %d .... local time 2 digit day in month
               %H .... local time 2 digit hour (24 hour clock)
               %M .... local time 2 digit minute
               %S .... local time 2 digit second
               %a .... local time abbreviated day in the week name
               %b .... local time abbreviated month name
               %z .... local time zone offset
               %gY ... GMT 4 digit year
               %gm ... GMT 2 digit month
               %gd ... GMT 2 digit day in month
               %gH ... GMT 2 digit hour (24 hour clock)
               %gM ... GMT 2 digit minute
               %gS ... GMT 2 digit second
               %ga ... GMT abbreviated day in the week name
               %gb ... GMT abbreviated month name
               %s .... log message
               %js ... JSON encoded log message (no enclosing quotes)
               %xs ... XML encoded log message
               %qs ... SQL encoded log message (no enclosing quotes)
               %cs ... CSV encoded log message (no enclosing quotes)
               %f .... source filename
               %jf ... JSON encoded filename (no enclosing quotes)
               %xf ... XML encoded filename
               %qf ... SQL encoded filename (no enclosing quotes)
               %cf ... CSV encoded filename (no enclosing quotes)
               %n .... line number
               %l .... "info" or "error"
               %L .... "INFO" or "ERROR"
               %t .... equivalent to '%Y-%m-%d %H:%M:%S%z'
               %T .... equivalent to '%Y-%m-%dT%H:%M:%S%z'
               %gt ... equivalent to '%gY-%gm-%gd %gH:%gM:%gSZ'
               %gT ... equivalent to '%gY-%gm-%gdT%gH:%gM:%gSZ'
               %h .... RFC 7231 IMF-fixdate: '%ga, %gd %gb %gY %gH:%gM:%gS GMT'
               %% .... outputs %

           --manual-logrotate          Pass this to enable manual log-rotation via 
                                       the logrotate service-runner command.
           --restart=WHEN

             Restart policy. Possible values for WHEN:
               NEVER ..... never restart (except when explicitely requesting restart
                           using the restart command)
               ALWAYS .... restart no matter if the service exited normally or with 
                           an error status.
               FAILURE ... (default) only restart the service if it exited with an 
                           error status or crashed.

       -u, --user=USER                 Run service as USER (name or UID).
       -g, --group=GROUP               Run service as GROUP (name or GID).
       -N, --priority=PRIORITY         Run service and service-runner(!) under 
                                       process scheduling priority PRIORITY. From -20
                                       (maximum priority) to +19 (minimum priority).
       -r, --rlimit=RES:SOFT[:HARD]

             Run service with given resource limits. This option can be defined 
             multiple times. SOFT/HARD may be an integer or "INFINITY". RES may be an
             integer or one of these names: AS, CORE, CPU, DATA, FSIZE, LOCKS, 
             MEMLOCK, MSGQUEUE, NICE, NOFILE, NPROC, RSS, RTPRIO, RTTIME, SIGPENDING,
             STACK

             Note that it is not checked if calling setrlimit() in the child process
             will succeed before forking the child. This means if it doesn't succeed
             there will be a crash-restart-loop.
             See: man setrlimit

       -k, --umask=UMASK               Run service with umask UMASK. Octal values 
                                       only.
       -C, --chdir=PATH                Change to directory PATH before running the 
                                       service. When --chroot is used chdir happens 
                                       after chroot. The service binary path is 
                                       relative to this PATH, even without "./" 
                                       prefix.
           --chroot=PATH               Call chroot with PATH before running the 
                                       service (and before calling chdir, if given).
                                       Unless --chdir is also given the service 
                                       binary path is relative to this PATH, even 
                                       without "./" prefix.
           --restart-sleep=SECONDS     Wait SECONDS before restarting service. 
                                       default: 1
           --crash-report=COMMAND
       -f, --foreground                Don't daemonize, but keep running in 
                                       foreground.

             Run `COMMAND NAME CODE STATUS LOGFILE` if the service crashed.
             CODE values:
               EXITED ... service has exited, STATUS is it's exit status
               KILLED ... service was killed, STATUS is the killing signal
               DUMPED ... service core dumped, STATUS is the killing signal

   service-runner stop <name> [options]

       Stop service <name>. If --pidfile was passed to the corresponding start 
       command it must be passed with the same argument here again.

   OPTIONS:
       -p, --pidfile=FILE              Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
           --shutdown-timeout=SECONDS  If the service doesn't shut down after SECONDS
                                       after sending SIGTERM send SIGKILL. -1 means 
                                       no timeout, just wait forever. default: -1

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

   service-runner logrotate <name> [options]

       Issue manual log-rotate to service-runner process. The service-runner has to 
       be started with the --manual-logrotate argument for this command to have any 
       effect.

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

       Print help message to <command>. If no command is passed, prints help message
       to all commands.

   service-runner version

       Print version string.

(c) 2022 Mathias Panzenb??ck
GitHub: https://github.com/panzi/service-runner```
