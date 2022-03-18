#!/usr/bin/bash

set -eo pipefail

function test_01_help_ok () {
    assert_ok "$SERVICE_RUNNER" help
}

function test_02_start_status_stop_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_run 0 "test is running" "" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_ok "$SERVICE_RUNNER" stop test --pidfile="$PIDFILE"
    assert_grep "received SIGTERM, exiting" "$LOGFILE"
    assert_run 3 "" "test is not running" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_03_restart_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" restart test --pidfile="$PIDFILE"
    assert_grep "service-runner: received signal .*, restarting service" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" status  test --pidfile="$PIDFILE"
    assert_ok   "$SERVICE_RUNNER" stop    test --pidfile="$PIDFILE"
    assert_grep "service-runner: received signal .*, forwarding to service PID" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status  test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_04_terminated_service () {
    local pid

    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    pid=$(cat "$PIDFILE")
    assert_ok kill -SIGTERM "$pid"
    sleep 1
    assert_grep "service-runner: test exited normally" "$LOGFILE"
    assert_grepv "service-runner: received signal 15, forwarding to service PID" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_05_killed_service () {
    local pid

    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    pid=$(cat "$PIDFILE")
    assert_ok kill -SIGKILL "$pid"
    sleep 1
    assert_grep "service-runner: test was killed by signal 9" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_06_failing_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/failing_service.sh 0
    sleep 1
    assert_grep "exiting now with status 1" "$LOGFILE"
    assert_grep "service-runner: test exited with error status 1" "$LOGFILE"
    sleep 1
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_07_crashing_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/crashing_service.sh 0
    sleep 1
    assert_grep "crashing now with SIGSEGV" "$LOGFILE"
    assert_grep "service-runner: test was killed by signal 11" "$LOGFILE"
    sleep 1
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_08_shutdown_timeout () {
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/refusing_to_terminate_service.sh
    sleep 0.5 # so that trap SIGTERM is for sure installed
    assert_ok   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE" --shutdown-timeout=5
    assert_grep "received SIGTERM, but ignores it" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_09_crash_report () {
    export CRASH_REPORT_FILE=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.crash_report.txt
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --crash-report=./tests/services/crash_reporter.sh ./tests/services/crashing_service.sh 0
    sleep 1
    assert_grep "Service test crashed!" "$CRASH_REPORT_FILE"
    assert_grep "Process crashed with signal: 11" "$CRASH_REPORT_FILE"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_10_logrotate () {
    local LOGFILE
    local logfile1
    local logfile2
    local logfile3
    local logfile4

    LOGFILE="/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.logrotate.%Y-%m-%d_%H-%M-%S.txt"
    logfile1=$(date -d '1 seconds' +"$LOGFILE")
    logfile2=$(date -d '2 seconds' +"$LOGFILE")
    logfile3=$(date -d '3 seconds' +"$LOGFILE")
    logfile4=$(date -d '4 seconds' +"$LOGFILE")
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 0.1
    sleep 5
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
    assert_grep message "$logfile1"
    assert_grep message "$logfile2"
    assert_grep message "$logfile3"
    assert_grep message "$logfile4"
}

function test_11_lsb_status () {
    assert_status 150 "$SERVICE_RUNNER" status test --pidfile=""
    echo 2147483646 > "$PIDFILE.runner"
    echo 2147483647 > "$PIDFILE"
    assert_status 1   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    rm -- "$PIDFILE.runner" "$PIDFILE"
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh
    assert_status 0 "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_ok       "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_status 3 "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_12_set_priority () {
    local pid
    local priority

    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --priority=5 ./tests/services/long_running_service.sh
    sleep 0.5
    pid=$(cat "$PIDFILE")
    priority=$(($(ps -o ni -h "$pid")))
    assert_ok test "$priority" -eq 5
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_13_set_illegal_priority () {
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --priority=+10 ./tests/services/long_running_service.sh
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner

    assert_fail "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --priority=+20 ./tests/services/long_running_service.sh
    assert_fail "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --priority=-21 ./tests/services/long_running_service.sh
    # XXX: I don't know how to produce a priority that fails on GitHub actions
    # assert_fail "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --priority=-20 ./tests/services/long_running_service.sh
}

function test_14_set_umask () {
    local pid
    local umask

    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --umask=345 ./tests/services/long_running_service.sh
    sleep 0.5
    pid=$(cat "$PIDFILE")
    umask=$(grep ^Umask: "/proc/$pid/status" | awk '{ print $2 }')
    assert_streq 0345 "$umask"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_15_rlimit_fsize () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --rlimit=fsize:4096 ./tests/services/creates_big_file.sh
    sleep 0.5
    assert_grep 'File size limit exceeded(core dumped)' "$LOGFILE"
    assert_grep 'test exited with error status 153' "$LOGFILE"

    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_16_set_illegal_rlimit () {
    assert_fail "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --rlimit=foo-bar-baz-illegal:4096 ./tests/services/long_running_service.sh
}

function test_17_logfile_not_limited_by_rlimit_fsize () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --rlimit=fsize:4096 ./tests/services/creates_big_log.sh
    sleep 0.5
    assert_ok "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"

    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_18_prevent_crash_restart_loop_before_exec () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --rlimit=nice:100 ./tests/services/creates_big_log.sh
    sleep 0.5
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_grep 'service-runner: received signal 15, forwarding to service PID' "$LOGFILE"
    assert_grep 'service-runner: test exited with error status 1' "$LOGFILE"
    assert_grep '(child) premature exit before execv() or failed execv() -> don'\''t restart' "$LOGFILE"
    assert_fail pgrep service-runner
}

function test_19_auto_restart_illegal_value () {
    assert_fail "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=xxx    -- "$SHELL" -c 'echo "illegal --restart value"'
}

function test_20_auto_restart_always_and_normal_exit () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=always -- "$SHELL" -c 'sleep 0.25; echo "always restart and normal exit"'
    sleep 1
    assert_grep "service-runner: test exited normally" "$LOGFILE"
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_21_auto_restart_always_and_fail () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=always -- "$SHELL" -c 'echo "always restart and fail"; exit 1'
    sleep 1.5
    assert_grep "service-runner: test exited with error status 1" "$LOGFILE"
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok test -e "$PIDFILE.runner"
    assert_ok kill -0 "$(cat "$PIDFILE.runner")"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_21_auto_restart_always_and_crash () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=always -- "$SHELL" -c 'echo "always restart and crash"; kill -SIGSEGV $$'
    sleep 1.5
    assert_grep "service-runner: test was killed by signal 11" "$LOGFILE"
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok test -e "$PIDFILE.runner"
    assert_ok kill -0 "$(cat "$PIDFILE.runner")"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_22_auto_restart_never_and_normal_exit () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=never -- "$SHELL" -c 'echo "never restart and normal exit"'
    sleep 0.5
    assert_grep "service-runner: test exited normally" "$LOGFILE"
    assert_grepv "service-runner: restarting test" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_22_auto_restart_never_and_fail () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=never -- "$SHELL" -c 'echo "never restart and fail"; exit 1'
    sleep 0.5
    assert_grep "service-runner: test exited with error status 1" "$LOGFILE"
    assert_grepv "service-runner: restarting test" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_22_auto_restart_never_and_crash () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=never -- "$SHELL" -c 'echo "never restart and crash"; kill -SIGSEGV $$'
    sleep 0.5
    assert_grep "service-runner: test was killed by signal 11" "$LOGFILE"
    assert_grepv "service-runner: restarting test" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_22_auto_restart_never_but_request_restart () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start   test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=never -- ./tests/services/long_running_service.sh 0.25
    sleep 0.5
    assert_ok   "$SERVICE_RUNNER" restart test --pidfile="$PIDFILE"
    sleep 0.5
    assert_grep "service-runner: test exited normally" "$LOGFILE"
    assert_grepv "service-runner: restarting test" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" status  test --pidfile="$PIDFILE"
    assert_ok   "$SERVICE_RUNNER" stop    test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status  test --pidfile="$PIDFILE"
}

function test_23_auto_restart_failure_and_normal_exit () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=failure -- "$SHELL" -c 'echo "restart on failure, but normal exit"'
    sleep 0.5
    assert_grep "service-runner: test exited normally" "$LOGFILE"
    assert_grepv "service-runner: restarting test" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_23_auto_restart_failure_and_fail () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=failure -- "$SHELL" -c 'sleep 0.25; echo "restart on failure and fail"; exit 1'
    sleep 1.5
    assert_grep "service-runner: test exited with error status 1" "$LOGFILE"
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}

function test_23_auto_restart_failure_and_crash () {
    assert_fail test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" --restart=failure -- "$SHELL" -c 'sleep 0.25; echo "restart on failure and crash"; kill -SIGSEGV $$'
    sleep 2
    assert_grep "service-runner: test was killed by signal 11" "$LOGFILE"
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok test -e "$PIDFILE.runner"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
}
