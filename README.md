service-runner
==============

My own service runner thing.

You might ask why? Surely there are enough such programs alread? Yeah, I felt
like it. Also, it is relatively minimal and does exactly what I want, which
includes:

* Start services
* Stop services
* Show service status
* Restarts the service on crash
* Can report crashes
* Start service as a different user
* Logfile handling including log-rotate and setting owner of logfile

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
       service-runner help [command]
       service-runner version

COMMANDS:

   service-runner start <name> [options] [--] <command> [argument...]

       Start <command> as service <name>. Does nothing if the service is already
       running. This automatically deamonizes, handles PID- and log-files, and 
       restarts on crash.

   OPTIONS:
       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
                                       Note that a second pidfile with the name
                                       FILE.runner is created containing the 
                                       process ID of the service-runner process
                                       itself.
       --logfile=FILE, -l FILE         Write service output to FILE. default: 
                                       /var/log/NAME-%%Y-%%m-%%d.log
                                       This implements log-rotating based on the
                                       file name pattern. See `man strftime` for
                                       a description of the pattern language.
       --chown-logfile                 Change owner of the logfile to user/group
                                       specified by --user/--group.
       --user=USER, -u USER            Run service as USER (name or UID).
       --group=GROUP, -g GROUP         Run service as GROUP (name or GID).
       --crash-sleep=SECONDS           Wait SECONDS before restarting service. 
                                       default: 1
       --crash-report=COMMAND          Run `COMMAND NAME CODE STATUS LOGFILE` if
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
       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid
       --shutdown-timeout=SECONDS      If the service doesn't shut down after 
                                       SECONDS after sending SIGTERM send 
                                       SIGKILL. 0 means no timeout, just wait 
                                       forever. default: 0

   service-runner restart <name> [options]

       Restart service <name>. Error if it's not already running.

   OPTIONS:
       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid

   service-runner status <name> [options]

       Print some status information about service <name>.

   OPTIONS:
       --pidfile=FILE, -p FILE         Use FILE as the pidfile. default: 
                                       /var/run/NAME.pid

   service-runner help [command]

       Print help message to <command>. If no command is passed, prints help 
       message to all commands.

   service-runner version

       Print version string.

(c) 2022 Mathias Panzenböck
GitHub: https://github.com/panzi/service-runner
```
