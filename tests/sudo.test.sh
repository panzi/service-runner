#!/usr/bin/bash

set -eo pipefail

# NOTE: Needs to be run as root and needs a known non-root user and group.
#       These can be passed as TEST_USER and TEST_GROUP environment variables.
#       Per default www-data will be used for both.

sudo_user=${TEST_USER:-www-data}
sudo_group=${TEST_GROUP:-www-data}

function test_01_user_group () {
    local expected_uid
    local expected_gid
    local GID
    local pid
    local actual_uid
    local actual_gid

    expected_uid=$(id -u "$sudo_user")
    expected_gid=$(id -g "$sudo_group")
    GID="$(stat -c %g "/proc/$$")"
    assert_ok test "$expected_uid" -ne "$UID"
    assert_ok test "$expected_gid" -ne "$GID"
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --user="$sudo_user" --group="$sudo_group" --logfile="$LOGFILE" ./tests/services/long_running_service.sh 1
    sleep 0.5
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
    local expected_uid
    local expected_gid

    expected_uid=$(id -u "$sudo_user")
    expected_gid=$(id -g "$sudo_group")
    assert_ok test "$expected_uid" -ne "$UID"
    assert_ok test "$expected_gid" -ne "$(stat -c %g "/proc/$$")"
    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --user="$sudo_user" --group="$sudo_group" --logfile="$LOGFILE" --chown-logfile ./tests/services/long_running_service.sh 1
    sleep 0.5
    assert_ok test "$(stat -c %u "$LOGFILE")" -eq "$expected_uid"
    assert_ok test "$(stat -c %g "$LOGFILE")" -eq "$expected_gid"
    assert_ok   "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_fail "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}

function test_03_status_as_user_of_root_service () {
    local expected_uid
    local expected_gid

    assert_ok "$SERVICE_RUNNER" start test --pidfile="$PIDFILE" --logfile="$LOGFILE" ./tests/services/long_running_service.sh
    sleep 0.5
    assert_status 0                                       "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_status 0 sudo -u "$sudo_user" -g "$sudo_group" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_ok                                             "$SERVICE_RUNNER" stop   test --pidfile="$PIDFILE"
    assert_status 3                                       "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_status 3 sudo -u "$sudo_user" -g "$sudo_group" "$SERVICE_RUNNER" status test --pidfile="$PIDFILE"
    assert_fail pgrep service-runner
}
