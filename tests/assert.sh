#!/usr/bin/bash

set -eo pipefail

export HIDE_CURSOR=$(echo -e '\033[?25l')
export SHOW_CURSOR=$(echo -e '\033[?25h')
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
    stdout_file=/tmp/service-runner.tests.assert_ok.$CURRENT_TEST_NUMBER.$$.stdout
    stderr_file=/tmp/service-runner.tests.assert_ok.$CURRENT_TEST_NUMBER.$$.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?
    stdout=$(cat "$stdout_file")
    stderr=$(cat "$stderr_file")

    if [[ "$status" -ne 0 ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_ok" >&2
        echo "command: $(quote_all "$@")" >&2
        echo "exit status: $status" >&2
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
}

function assert_fail () {
    stdout_file=/tmp/service-runner.tests.assert_fail.$CURRENT_TEST_NUMBER.$$.stdout
    stderr_file=/tmp/service-runner.tests.assert_fail.$CURRENT_TEST_NUMBER.$$.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?
    stdout=$(cat "$stdout_file")
    stderr=$(cat "$stderr_file")

    if [[ "$status" -eq 0 ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_fail" >&2
        echo "command: $(quote_all "$@")" >&2
        echo "exit status: $status" >&2
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
}

function assert_streq () {
    if [[ "$1" != "$2" ]]; then
        echo "assertion failed in $CURRENT_TEST: assert_streq" >&2
        diff --color=always -u <(echo -n "$1") <(echo -n "$2") >&2
        return 1
    fi
}

function assert_grep () {
    pattern=$1
    filename=$2

    if ! grep -q -- "$pattern" "$filename"; then
        echo "assertion failed in $CURRENT_TEST: assert_file_grep" >&2
        echo "pattern:  $(sh_quote "$pattern")"
        echo "filename: $(sh_quote "$filename")"
        return 1
    fi
}

function assert_grepv () {
    pattern=$1
    filename=$2

    if ! grep -q -v -- "$pattern" "$filename"; then
        echo "assertion failed in $CURRENT_TEST: assert_file_grepv" >&2
        echo "pattern:  $(sh_quote "$pattern")"
        echo "filename: $(sh_quote "$filename")"
        return 1
    fi
}

function assert_run () {
    expected_status=$1
    expected_stdout=$2
    expected_stderr=$3

    shift 3

    stdout_file=/tmp/service-runner.tests.$CURRENT_TEST_NUMBER.$$.stdout
    stderr_file=/tmp/service-runner.tests.$CURRENT_TEST_NUMBER.$$.stderr
    status=0
    "$@" 1>"$stdout_file" 2>"$stderr_file" || status=$?
    stdout=$(cat "$stdout_file")
    stderr=$(cat "$stderr_file")

    assert_status=0
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

    return "$assert_status"
}
