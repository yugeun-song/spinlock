#!/usr/bin/env bash
exec cmake -D "BUILD_SCRIPT=$(basename -- "$0")" -P "$(dirname -- "$0")/build.cmake" -- "$@"