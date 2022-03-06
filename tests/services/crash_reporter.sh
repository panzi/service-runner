#!/usr/bin/bash

SELF=$(readlink -f "$0")
DIR=$(dirname "$SELF")

service_name=$1
exit_code=$2
exit_status=$3
logfile=$4

echo "[$(date +'%Y-%m-%d %H:%M:%S%z')] CRASH REPORT:" "$@"

{
    headline="Service $service_name crashed!"
    echo "$headline"
    echo "${headline//?/=}"

    echo "Timestamp: $(date +'%Y-%m-%d %H:%M:%S%z')"
    case "$exit_code" in
        EXITED)
            echo "Process exited with status: $exit_status"
            ;;

        KILLED)
            echo "Process killed with signal: $exit_status"
            ;;

        DUMPED)
            echo "Process crashed with signal: $exit_status"
            ;;
    esac

    echo
    echo "Last 50 lines of $logfile:"
    echo
    tail -n 50 "$logfile"
} > "${CRASH_REPORT_FILE:-$DIR/../tmp/crash_report-$(date +'%Y-%m-%d_%H-%M-%S%z').txt}"
