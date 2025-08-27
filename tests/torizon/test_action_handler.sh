#!/bin/bash

LOCATION=$(dirname "${BASH_SOURCE[0]}")

# shellcheck disable=SC1090
source "${LOCATION}/test_common.sh"

handle_command "$1"
