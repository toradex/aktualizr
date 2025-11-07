#! /bin/bash

# Optional input variables:
#
# - STEPS: what steps to execute; see parsing below for possible values.
# - TEST_BLACKLIST: space/newline separated list of tests not to
#   run, e.g. "test_ostree_custom_uri test_misc_ostree_update".
#
# Other useful variables (understood by ctest):
#
# - CTEST_PARALLEL_LEVEL
# - CTEST_OUTPUT_ON_FAILURE

set -euo pipefail

STEPS=${STEPS-all}

gen_buildsys_=$([[ " ${STEPS} " =~ ( gen-buildsys | build | all ) ]] && echo "1" || echo "0")
bld_default_=$([[ " ${STEPS} " =~ ( build-default | build | all ) ]] && echo "1" || echo "0")
bld_tests_=$([[ " ${STEPS} " =~ ( build-tests | build | all ) ]] && echo "1" || echo "0")
run_tests_=$([[ " ${STEPS} " =~ ( run-tests | test | all) ]] && echo "1" || echo "0")
run_check_format_=$([[ " ${STEPS} " =~ ( check-format | qa | all) ]] && echo "1" || echo "0")
run_clang_tidy_=$([[ " ${STEPS} " =~ ( tidy | qa | all ) ]] && echo "1" || echo "0")
tst_blacklist_="${TEST_BLACKLIST-}"

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
