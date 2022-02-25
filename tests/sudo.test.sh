#!/usr/bin/bash

set -eo pipefail

# TODO: tests using --user/--group and --chown-logfile
#       needs to be run as root and needs a known non-root user

function test_01_user_group () {
    user=${TEST_USER:-www-data}
    group=${TEST_GROUP:-www-data}
    expected_uid=$(id -u "$user")
    expected_gid=$(id -g "$group")
    GID="$(stat -c %g /proc/$$)"
    assert_ok test "$expected_uid" -ne "$UID"
    assert_ok test "$expected_gid" -ne "$GID"
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --user="$user" --group="$group" --logfile="$LOGFILE" ./examples/long_running_service.sh 1
    assert_ok "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    pid=$(cat -- "$PIDFILE")
    assert_ok test -n "$pid"
    actual_uid=$(stat -c %u "/proc/$pid")
    actual_gid=$(stat -c %g "/proc/$pid")
    assert_ok test "$actual_uid" -eq "$expected_uid"
    assert_ok test "$actual_gid" -eq "$expected_gid"
    assert_ok test "$(stat -c %u "$LOGFILE")" -eq "$UID"
    assert_ok test "$(stat -c %g "$LOGFILE")" -eq "$GID"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_02_chown_logfile () {
    user=${TEST_USER:-www-data}
    group=${TEST_GROUP:-www-data}
    expected_uid=$(id -u "$user")
    expected_gid=$(id -g "$group")
    assert_ok test "$expected_uid" -ne "$UID"
    assert_ok test "$expected_gid" -ne "$(stat -c %g /proc/$$)"
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --user="$user" --group="$group" --logfile="$LOGFILE" --chown-logfile ./examples/long_running_service.sh 1
    assert_ok test "$(stat -c %u "$LOGFILE")" -eq "$expected_uid"
    assert_ok test "$(stat -c %g "$LOGFILE")" -eq "$expected_gid"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}
