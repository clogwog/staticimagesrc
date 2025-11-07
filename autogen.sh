#!/usr/bin/env bash

set -euo pipefail

autoreconf -fi

if [ ! -f configure ]; then
    echo "configure script not generated" >&2
    exit 1
fi

if [ "${NOCONFIGURE:-}" != "1" ]; then
    ./configure "$@"
fi


