#!/usr/bin/bash

set -eo pipefail

function handle_sigterm () {
    echo "received SIGTERM, exiting..."
    exit
}

trap handle_sigterm SIGTERM

echo "$0 started"
sleep 5
echo "$0: crashing now with SIGSEGV...">&2
kill -SIGSEGV $$
