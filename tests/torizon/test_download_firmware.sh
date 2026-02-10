#!/bin/bash
# Script for testing the download-install action (handler_downloads_firmware mode).
# Expects env vars: SECONDARY_TARGET_URI, SECONDARY_FIRMWARE_SHA256, SECONDARY_FIRMWARE_LENGTH,
# SECONDARY_UPDATE_TYPE, SECONDARY_CUSTOM_METADATA, SECONDARY_TARGET_FILENAME, plus SHARED_VARS.

LOCATION=$(dirname "${BASH_SOURCE[0]}")

# shellcheck disable=SC1090,SC1091
source "${LOCATION}/test_common.sh"

if [ "$1" != "download-install" ]; then
    exit 65
fi

DOWNLOAD_FIRMWARE_VARS=(
  "SECONDARY_TARGET_URI"
  "SECONDARY_FIRMWARE_SHA256"
  "SECONDARY_FIRMWARE_LENGTH"
  "SECONDARY_UPDATE_TYPE"
  "SECONDARY_CUSTOM_METADATA"
  "SECONDARY_TARGET_FILENAME"
)

for vn in "${SHARED_VARS[@]}" "${DOWNLOAD_FIRMWARE_VARS[@]}"; do
    if [ -z "${!vn}" ]; then
        exit 65
    fi
done

if [ -n "${TEST_COMMAND}" ]; then
    handle_command "$TEST_COMMAND"
fi

if [ -n "${TEST_JSON_OUTPUT}" ]; then
    echo "$TEST_JSON_OUTPUT"
else
    echo '{"status": "ok"}'
fi
exit 0
