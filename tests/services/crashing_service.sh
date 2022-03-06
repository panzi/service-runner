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
sleep "$wait"
printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: crashing now with SIGSEGV...\n' -1 "$name">&2
kill -SIGSEGV $$
