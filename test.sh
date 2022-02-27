#!/usr/bin/bash

set -eo pipefail

SELF=$(realpath -- "$0")
DIR=$(dirname -- "$SELF")

cd -- "$DIR"

export MAX_COLS=80
export MAX_TRY=6
export SERVICE_RUNNER=./build/bin/service-runner

. tests/assert.sh

if pids=$(pgrep service-runner); then
    echo "${RED}service-runner procress is active!${NORMAL}" >&2
    echo "$pids" | sed 's/^/    /' >&2
    echo "You might want to run the tests in an isolated environment like docker or chroot." >&2
    exit 1
fi

function after_test () {
    if [[ -n "$PIDFILE" ]]; then
        if [[ -e "$PIDFILE.runner" ]]; then
            echo
            echo "${RED}pidfile remained after test: $PIDFILE.runner${NORMAL}" >&2
            pid=$(cat "$PIDFILE.runner" 2>/dev/null || true)
            if [[ -n "$pid" ]]; then
                kill -SIGTERM "$pid" || true
                n=0
                while kill -0 "$pid" 2>/dev/null; do
                    if [[ "$n" -ge "$MAX_TRY" ]]; then
                        echo "${RED}process $pid did not go away!${NORMAL}" >&2
                        exit 1
                    fi
                    sleep 1
                    n=$((n+1))
                done
            fi
        fi

        if [[ -e "$PIDFILE" ]]; then
            echo
            echo "${RED}pidfile remained after test: $PIDFILE${NORMAL}" >&2
            pid=$(cat "$PIDFILE" 2>/dev/null || true)
            if [[ -n "$pid" ]]; then
                kill -SIGTERM "$pid" || true
                n=0
                while kill -0 "$pid" 2>/dev/null; do
                    if [[ "$n" -ge "$MAX_TRY" ]]; then
                        echo "${RED}process $pid did not go away!${NORMAL}" >&2
                        exit 1
                    fi
                    sleep 1
                    n=$((n+1))
                done
            fi
        fi

        if pids=$(pgrep service-runner); then
            echo
            echo "${RED}service-runner procress remained active after test! sending SIGTERM...${NORMAL}" >&2
            kill -SIGTERM $pids || true
            n=0
            while kill -0 $pids 2>/dev/null; do
                if [[ "$n" -ge "$MAX_TRY" ]]; then
                    echo "${RED}There are still service-runner procresses, giving up:${NORMAL}" >&2
                    echo "$pids" | sed 's/^/    /' >&2
                    exit 1
                fi
                sleep 1
                n=$((n+1))
            done
        fi
    fi
}

function cleanup () {
    after_test
    echo -n "$SHOW_CURSOR"
}

trap cleanup EXIT
echo -n "$HIDE_CURSOR"

fail_count=0
success_count=0
test_count=0

function run_test_suit () {
    local test_file=$1
    local prefix_len
    local cols
    local prefix
    local test_name
    local test_count

    TEST_SUIT=$(basename "$test_file" .test.sh)
    export TEST_SUIT

    shift

    if [[ -t 1 ]] && [[ -t 2 ]]; then
        isatty=1
        cols=$(tput cols 2>/dev/null || true)
        if [[ -z "$cols" ]] || [[ "$cols" -gt "$MAX_COLS" ]]; then
            cols=$MAX_COLS
        fi
        cr='\r'
    else
        isatty=0
        cols=$MAX_COLS
        cr=
    fi

    for test_func in "$@"; do
        test_name=${test_func//_/ }
        test_count=$((test_count+1))

        prefix="- $test_name "
        if [[ "$isatty" -eq 1 ]]; then
            echo -n "$prefix"
        fi
        prefix_len=$(echo -n "$prefix" | wc -c)
        export CURRENT_TEST_NUMBER=$test_count
        export CURRENT_TEST=$test_file:$test_func
        export CURRENT_TEST_NAME=$test_name

        export PIDFILE=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.pid
        export LOGFILE=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.log
        if [[ -e "$LOGFILE" ]]; then
            rm -- "$LOGFILE"
        fi

        if out=$(bash -eo pipefail -c ". tests/assert.sh; . $(printf %q "$test_file"); $(printf %q "test_$test_func")" 2>&1); then
            success_count=$((success_count+1))
            printf -- "$cr%s${GREEN}%$((cols-prefix_len))s${NORMAL}\n" "$prefix" PASS

            if [[ -e "$LOGFILE" ]]; then
                rm -- "$LOGFILE"
            fi
        else
            fail_count=$((fail_count+1))
            printf -- "$cr%s${RED}%$((cols-prefix_len))s${NORMAL}\n" "$prefix" FAIL
            echo "$out"
            echo
        fi

        after_test

        unset PIDFILE
        unset LOGFILE
        export PIDFILE
        export LOGFILE
    done
}

function run_test_file () {
    local suit_name
    local test_funcs
    local test_file=$1
    shift

    suit_name=$(basename "$test_file" .test.sh | tr _ ' ')
    echo "$suit_name"
    echo "${suit_name//?/=}"

    if [[ $# -eq 0 ]]; then
        test_funcs=$(bash -eo pipefail -c ". $(printf %q "$test_file"); declare -F" | sed 's/^declare -f //' | grep ^test_ | sed 's/^test_//' | sort || true)

        run_test_suit "$test_file" $test_funcs
    else
        run_test_suit "$test_file" "$@"
    fi

    echo
}

if [[ $# -eq 0 ]]; then
    for test_file in tests/*.test.sh; do
        run_test_file "$test_file"
    done
else
    run_test_file "$@"
fi

if [[ "$fail_count" -eq 0 ]]; then
    fail_pre=
    fail_post=
    ok_pre=$GREEN
    ok_post=$NORMAL
else
    fail_pre=$RED
    fail_post=$NORMAL
    ok_pre=
    ok_post=
fi

echo "tests: $test_count, failed: $fail_pre$fail_count$fail_post, successful: $ok_pre$success_count$ok_post"
if [[ "$fail_count" -ne 0 ]]; then
    exit 1
fi
