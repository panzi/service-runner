#!/usr/bin/bash

set -eo pipefail

name=$(basename "$0" .sh)

function handle_sigterm () {
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s received SIGTERM, exiting...\n' -1 "$name"
    exit
}

trap handle_sigterm SIGTERM

printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s started\n' -1 "$name"
sleep 5
printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: exiting now with status 1...\n' -1 "$name">&2
exit 1
