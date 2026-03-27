#!/usr/bin/env bash
set -euo pipefail

# One-click build → run → cleanup for rdma_init_test
# Location: repo root. It will use src/Makefile if present,
# otherwise fallback to invoking gcc directly.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/src"

if [ -f Makefile ]; then
    make run
else
    gcc 2_1_rdma_init_test.c -o rdma_init_test -libverbs
    ./rdma_init_test
    rm -f rdma_init_test
fi
