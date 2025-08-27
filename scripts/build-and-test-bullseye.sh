#! /bin/bash

# Optional input variables:
#
# - GEN_BUILDSYS: set to "1" to generate the build system files.
# - BLD_DEFAULT: set to "1" to build the default targets.
# - BLD_TESTS: set to "1" to build the tests.
# - RUN_TESTS: set to "1" to run the tests.
# - RUN_CHECK_FORMAT: set to "1" to run "check-format".
# - RUN_CLANG_TIDY:  set to "1" to run "clang-tidy".
# - TST_BLACKLIST: blacklisted tests (regex), e.g.
#                  "test_ostree_custom_uri test_misc_ostree_update"
#
# Other useful variables (understood by ctest):
#
# - CTEST_PARALLEL_LEVEL
# - CTEST_OUTPUT_ON_FAILURE

set -euo pipefail

# TODO: Fix the errors with these tests.
tst_blacklist_="
  test_aktualizr
  test_aktualizr_update_lock
  test_uptane
  test_uptane_delegation
  test_uptane_network
  test_uptane_cancellation
  test_uptane_update_failure
  test_c_api
  aktualizr-option-version
  test_command_runner
  test-help-with-other-options
  test-help-with-nonexistent-options
  test_ip_secondary
  test_ip_secondary_rotation
  test_customrepo_failure
  test_ip_secondary_ostree
  test_treehub_failure
  test_misc_ostree_update
  test_ostree_custom_uri
  test_install_aktualizr_and_update
"

gen_buildsys_="${GEN_BUILDSYS:-1}"
bld_default_="${BLD_DEFAULT:-1}"
bld_tests_="${BLD_TESTS:-1}"
run_tests_="${RUN_TESTS:-1}"
run_check_format_="${RUN_CHECK_FORMAT:-1}"
run_clang_tidy_="${RUN_CLANG_TIDY:-1}"
tst_blacklist_="${TST_BLACKLIST-${tst_blacklist_}}"

tmpdir() {
    export TMPDIR="${1}"
    echo "Setting temp directory to: ${TMPDIR}"
    mkdir -p "${TMPDIR}"
}

# This expects to find the checked out source code in ./source
# It builds it into ./build and runs the tests.
# Test with
# docker build -t aktualizr-bullseye -f docker/Dockerfile.debian.bullseye .
# docker run --mount=type=volume,source=ccache,destination=/home/testuser/.cache -it aktualizr-bullseye source/scripts/build-and-test.sh

if [ "${gen_buildsys_}" = "1" ] || [ ! -e "build/build.ninja" ]; then
    echo -e "\n== Generating build.ninja ==\n"
    cmake -G Ninja -S source -B build \
      -DBUILD_SOTA_TOOLS=ON \
      -DBUILD_OSTREE=ON \
      -DBUILD_P11=ON \
      -DPKCS11_ENGINE_PATH=/usr/lib/x86_64-linux-gnu/engines-1.1/libpkcs11.so \
      -DTEST_PKCS11_MODULE_PATH=/usr/lib/softhsm/libsofthsm2.so \
      -DFAULT_INJECTION=ON
fi

SRC_DIR=$(pwd)/source
BLD_DIR=$(pwd)/build

if [ "${run_check_format_}" = "1" ]; then
    cd "${BLD_DIR}"
    echo -e "\n== Make 'check-format' ==\n"
    time ninja -v check-format
fi

if [ "${bld_default_}" = "1" ]; then
    cd "${BLD_DIR}"
    tmpdir "${BLD_DIR}/tmp"
    echo -e "\n== Building default targets ==\n"
    time ninja -v
fi

if [ "${bld_tests_}" = "1" ]; then
    cd "${BLD_DIR}"
    tmpdir "${BLD_DIR}/tmp"
    echo -e "\n== Building tests ==\n"
    time ninja -v build_tests
fi

if [ "${run_tests_}" = "1" ]; then
    # Run the tests in the build directory but keep the tmp directory inside the source directory to
    # avoid errors in CI due to attempts to make links across devices.
    cd "${BLD_DIR}"
    tmpdir "${SRC_DIR}/tmp"

    echo -e "\n== Preparing to run tests ==\n"
    export SOFTHSM2_CONF=../source/tests/test_data/softhsm2.conf
    export TOKEN_DIR=${TMPDIR?TMPDIR not set}/tokens
    rm -fr "${TOKEN_DIR}" && mkdir "${TOKEN_DIR}"
    ../source/scripts/setup_hsm.sh

    if [ -n "${tst_blacklist_}" ]; then
        # Translate space-separated list into a regex.
	# shellcheck disable=SC2086
        tst_blacklist_=$(echo ${tst_blacklist_} | \
                             sed -e 's/[[:space:]]\+/|/g' \
                                 -e 's/^/(/' \
                                 -e 's/$/)/')
    fi

    export OSTREE_SYSROOT_DEBUG="mutable-deployments"
    echo -e "\n== Running tests ==\n"
    ctest ${tst_blacklist_:+-E "${tst_blacklist_}"} -V
fi

if [ "${run_clang_tidy_}" = "1" ]; then
    cd "${BLD_DIR}"
    echo -e "\n== Make 'clang-tidy' ==\n"
    time ninja -v clang-tidy -k 0 -j "$(nproc)"
fi

echo -e "\n== DONE! ==\n"
