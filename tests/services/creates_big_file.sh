#!/usr/bin/bash

set -eo pipefail

name=$(basename "$0" .sh)
size=${1:-5242880}

function handle_sigterm () {
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s received SIGTERM, exiting...\n' -1 "$name"
    exit
}

trap handle_sigterm SIGTERM

printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s started\n' -1 "$name"

i=1
while true; do
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: creating big file %d\n' -1 "$name" "$i"
    head -c "$size" /dev/random > "/tmp/service-runner.creates_big_file.$$.$i.bin"

    sleep 10
    i=$((i+1))
done
