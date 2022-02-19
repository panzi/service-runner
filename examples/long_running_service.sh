#!/usr/bin/bash

set -eo pipefail

name=$(basename "$0" .sh)

function handle_sigterm () {
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s received SIGTERM, exiting...\n' -1 "$name"
    exit
}

trap handle_sigterm SIGTERM

printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s started\n' -1 "$name"

while true; do
    if [[ "$RANDOM" -gt 16383 ]]; then
        printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: [INFO] message\n' -1 "$name"
    else
        printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: [ERROR] message\n' -1 "$name">&2
    fi

    sleep 5
done
