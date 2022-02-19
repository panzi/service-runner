#!/usr/bin/bash

set -eo pipefail

function handle_sigterm () {
    echo "received SIGTERM, exiting..."
    exit
}

trap handle_sigterm SIGTERM

echo "$0 started"

while true; do
    if [[ "$RANDOM" -gt 16383 ]]; then
        echo "$0: stdout message"
    else
        echo "$0: stderr message">&2
    fi

    sleep 5
done
