#!/usr/bin/bash

set -eo pipefail

function test_01_help_ok () {
    assert_ok "$SERVICE_RUNNER" help
}

function test_02_start_status_stop_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_run 0 "test is running" "" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_ok "$SERVICE_RUNNER" stop test --pidfile="$PIDFILE"
    assert_grep "received SIGTERM, exiting" "$LOGFILE"
    assert_run 1 "" "test is not running" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_03_restart_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/long_running_service.sh 0.5
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
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/long_running_service.sh 0.5
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
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/long_running_service.sh 0.5
    sleep 0.5
    assert_grep "message" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    pid=$(cat "$PIDFILE")
    assert_ok kill -SIGKILL "$pid"
    sleep 1
    assert_grep "service-runner: \*\*\* error: test was killed by signal 9" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_06_failing_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/failing_service.sh 0
    sleep 1
    assert_grep "exiting now with status 1" "$LOGFILE"
    assert_grep "service-runner: \*\*\* error: test exited with error status 1" "$LOGFILE"
    sleep 1
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_07_crashing_service () {
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/crashing_service.sh 0
    sleep 1
    assert_grep "crashing now with SIGSEGV" "$LOGFILE"
    assert_grep "service-runner: \*\*\* error: test was killed by signal 11" "$LOGFILE"
    sleep 1
    assert_grep "service-runner: restarting test" "$LOGFILE"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_08_shutdown_timeout () {
    assert_ok   "$SERVICE_RUNNER" start  test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./examples/refusing_to_terminate_service.sh
    sleep 0.5 # so that trap SIGTERM is for sure installed
    assert_ok   "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE" --shutdown-timeout=5
    assert_grep "received SIGTERM, but ignores it" "$LOGFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

# TODO: more tests, like:
# test_crash_report
# test_logrotate
