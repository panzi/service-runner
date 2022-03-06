#!/bin/bash

### BEGIN INIT INFO
# Provides:             An Example Service
# Required-Start:       $remote_fs $syslog
# Required-Stop:        $remote_fs $syslog
# Default-Start:        2 3 4 5
# Default-Stop:         
# Short-Description:    An example service for service-runner
### END INIT INFO

set -eo pipefail

. /lib/lsb/init-functions

export PATH="${PATH:+$PATH:}/usr/sbin:/sbin"

name="example-service"
daemon=(./tests/services/long_running_service.sh) # YOUR SERVICE
user=www-data
group=www-data
runner="/usr/local/bin/service-runner"
crash_report="./tests/services/crash_reporter.sh"
logfile="/var/log/example-%Y-%m-%d.log"

case "$1" in
    start)
        log_success_msg "$name: Starting example service" "$name"
        "$runner" start "$name" \
            --user "$user" --group "$group" \
            --logfile "$logfile" \
            --crash-report="$crash_report" \
            -- "${daemon[@]}"
        ;;

    stop)
        log_success_msg "$name: Stopping example service" "$name"
        "$runner" stop "$name"
        ;;

    restart)
        log_success_msg "$name: Restarting example service" "$name"
        "$runner" status "$name" >/dev/null 2>&1 && "$runner" stop "$name"
        "$runner" start "$name" \
            --user "$user" --group "$group" \
            --logfile "$logfile" \
            --crash-report="$crash_report" \
            -- "${daemon[@]}"
        ;;

    status)
        "$runner" status "$name"
        ;;

    *)
        log_failure_msg "Usage: /etc/init.d/example-service {start|stop|restart|status}" || true
        exit 1
esac
