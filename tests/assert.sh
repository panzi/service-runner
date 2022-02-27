#!/usr/bin/bash

set -eo pipefail

if [[ -t 1 ]] && [[ -t 2 ]]; then
    HIDE_CURSOR=$(echo -e '\033[?25l')
    SHOW_CURSOR=$(echo -e '\033[?25h')
else
    HIDE_CURSOR=
    SHOW_CURSOR=
fi

export HIDE_CURSOR
export SHOW_CURSOR

# GitHib actions supports colors:
export RED=$(echo -e '\033[0;1;31m')
export GREEN=$(echo -e '\033[0;1;32m')
export NORMAL=$(echo -e '\033[0m')

function sh_quote () {
    printf "'%s' " "${1//\'/\'\\\'\'}"
}

function quote_all () {
    for arg in "$@"; do
        printf "'%s' " "${arg//\'/\'\\\'\'}"
    done
}

function assert_ok () {
    local stdout_file
    local stderr_file
    local status
    local stdout
    local stderr

    stdout_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_ok.stdout
    stderr_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_ok.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?

    if [[ "$status" -ne 0 ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_ok" >&2
        echo "command: $(quote_all "$@")" >&2
        echo "exit status: $status" >&2
        stdout=$(cat "$stdout_file")
        stderr=$(cat "$stderr_file")
        if [[ "$stdout" != "" ]]; then
            echo "stdout:"
            echo "${RED}$stdout${NORMAL}" | sed 's/^/    /' >&2
        fi
        if [[ "$stderr" != "" ]]; then
            echo "stderr:"
            echo "${RED}$stderr${NORMAL}" | sed 's/^/    /' >&2
        fi
        return 1
    fi

    rm -- "$stdout_file" "$stderr_file" 2>/dev/null || true
}

function assert_fail () {
    local stdout_file
    local stderr_file
    local status
    local stdout
    local stderr

    stdout_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_fail.stdout
    stderr_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_fail.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?

    if [[ "$status" -eq 0 ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_fail" >&2
        echo "command: $(quote_all "$@")" >&2
        echo "exit status: $status" >&2
        stdout=$(cat "$stdout_file")
        stderr=$(cat "$stderr_file")
        if [[ "$stdout" != "" ]]; then
            echo "stdout:"
            echo "${RED}$stdout${NORMAL}" | sed 's/^/    /' >&2
        fi
        if [[ "$stderr" != "" ]]; then
            echo "stderr:"
            echo "${RED}$stderr${NORMAL}" | sed 's/^/    /' >&2
        fi
        return 1
    fi

    rm -- "$stdout_file" "$stderr_file" 2>/dev/null || true
}

function assert_status () {
    local stdout_file
    local stderr_file
    local status
    local stdout
    local stderr
    local expected_status=$1
    shift

    stdout_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_status.stdout
    stderr_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_status.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?

    if [[ "$status" -ne "$expected_status" ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_status" >&2
        echo "command: $(quote_all "$@")" >&2
        echo "expected exit status: $expected_status" >&2
        echo "actual exit status:   $status" >&2
        stdout=$(cat "$stdout_file")
        stderr=$(cat "$stderr_file")
        if [[ "$stdout" != "" ]]; then
            echo "stdout:"
            echo "${RED}$stdout${NORMAL}" | sed 's/^/    /' >&2
        fi
        if [[ "$stderr" != "" ]]; then
            echo "stderr:"
            echo "${RED}$stderr${NORMAL}" | sed 's/^/    /' >&2
        fi
        return 1
    fi

    rm -- "$stdout_file" "$stderr_file" 2>/dev/null || true
}

function assert_streq () {
    if [[ "$1" != "$2" ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_streq" >&2
        diff --color=always -u <(echo -n "$1") <(echo -n "$2") >&2
        return 1
    fi
}

function assert_grep () {
    local pattern=$1
    local filename=$2

    if ! grep -q -- "$pattern" "$filename"; then
        echo "assertion failed in $CURRENT_TEST: assert_file_grep" >&2
        echo "pattern:  $(sh_quote "$pattern")"
        echo "filename: $(sh_quote "$filename")"
        return 1
    fi
}

function assert_grepv () {
    local pattern=$1
    local filename=$2

    if ! grep -q -v -- "$pattern" "$filename"; then
        echo "assertion failed in $CURRENT_TEST: assert_file_grepv" >&2
        echo "pattern:  $(sh_quote "$pattern")"
        echo "filename: $(sh_quote "$filename")"
        return 1
    fi
}

function assert_run () {
    local stdout_file
    local stderr_file
    local status
    local stdout
    local stderr

    local expected_status=$1
    local expected_stdout=$2
    local expected_stderr=$3
    local assert_status=0

    shift 3

    stdout_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_run.stdout
    stderr_file=/tmp/service-runner.tests.$TEST_SUIT.$CURRENT_TEST_NUMBER.$$.assert_run.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?
    stdout=$(cat "$stdout_file")
    stderr=$(cat "$stderr_file")

    if [[ "$status" -ne "$expected_status" ]]; then
        if [[ "$assert_status" -eq 0 ]]; then
            echo "assertion failed in $CURRENT_TEST: assert_run $*" >&2
            assert_status=1
        fi
        echo "expected status: $expected_status" >&2
        echo "actual status:   ${RED}$status${NORMAL}" >&2
    fi

    if [[ "$stdout" != "$expected_stdout" ]]; then
        if [[ "$assert_status" -eq 0 ]]; then
            echo "assertion failed in $CURRENT_TEST: assert_run $*" >&2
            assert_status=1
        fi
        echo "stdout missmatch:" >&2
        diff --color=always -u <"$stdout_file" <(echo -n "$stdout") | sed 's/^/    /' >&2
    fi

    if [[ "$stderr" != "$expected_stderr" ]]; then
        if [[ "$assert_status" -eq 0 ]]; then
            echo "assertion failed in $CURRENT_TEST: assert_run $*" >&2
            assert_status=1
        fi
        echo "stderr missmatch:" >&2
        diff --color=always -u <"$stderr_file" <(echo -n "$stderr") | sed 's/^/    /' >&2
    fi

    if [[ "$assert_status" -eq 0 ]]; then
        rm -- "$stdout_file" "$stderr_file" 2>/dev/null || true
    fi

    return "$assert_status"
}
