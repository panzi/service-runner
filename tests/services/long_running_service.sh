#!/usr/bin/bash

set -eo pipefail

name=$(basename "$0" .sh)
wait=${1:-5}

function handle_sigterm () {
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s received SIGTERM, exiting...\n' -1 "$name"
    exit
}

trap handle_sigterm SIGTERM

printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s started\n' -1 "$name"

i=0
while true; do
    if [[ "$((i%3))" -eq 0 ]]; then
        printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: [ERROR] message\n' -1 "$name">&2
    else
        printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: [INFO] message\n' -1 "$name"
    fi

    sleep "$wait"
    i=$((i+1))
done
