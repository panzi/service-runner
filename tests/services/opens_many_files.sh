#!/usr/bin/bash

set -eo pipefail

name=$(basename "$0" .sh)
wait=${1:-0.5}

function handle_sigterm () {
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s received SIGTERM, exiting...\n' -1 "$name"
    exit
}

trap handle_sigterm SIGTERM

printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s started\n' -1 "$name"

function file_writer () {
    local i=1
    while true; do
        printf '[%(%Y-%m-%d %H:%M:%S%z)T] keep alive %d\n' -1 "$i"
        sleep 10
        i=$((i+1))
    done
}

i=1
while true; do
    printf '[%(%Y-%m-%d %H:%M:%S%z)T] %s: opening file %d\n' -1 "$name" "$i"
    file_writer > "/tmp/service-runner.opens_many_files.$$.$i.txt" &

    sleep "$wait"
    i=$((i+1))
done
