#!/usr/bin/bash

set -eo pipefail

echo "$0 started"
sleep 5
echo "$0: crashing now with SIGSEGV...">&2
kill -SIGSEGV $$
