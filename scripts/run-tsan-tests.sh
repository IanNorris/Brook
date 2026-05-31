#!/usr/bin/env bash
#
# run-tsan-tests.sh — build and run the host lock/concurrency tests under
# ThreadSanitizer.
#
# TSan is the correct race detector for Brook's host tests because it models
# C++11 std::atomic as synchronisation (valgrind/helgrind does not, and
# false-positives on every lock-free path). This is the deterministic race
# gate that guards the concurrency-bug class found in the architectural review.
#
# Run it on a normal dev box or CI runner:
#     ./scripts/run-tsan-tests.sh
#
# NOTE on sandboxes: TSan disables ASLR at startup via the personality(2)
# syscall. Containers/sandboxes that block that syscall (seccomp ENOSYS) cannot
# *execute* TSan binaries — the build still succeeds, but the run aborts with
# a CHECK on tsan_platform_linux.cpp. This script detects that case and exits
# with a clear, non-confusing message rather than a raw sanitizer crash.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/tsan_host_tests"
HOST_TESTS_SRC="${REPO_ROOT}/src/tests/host"

CXX_BIN="${HOST_CXX:-${CXX:-c++}}"

echo "==> ThreadSanitizer host-test gate"
echo "    compiler : ${CXX_BIN}"
echo "    build dir: ${BUILD_DIR}"

cmake -S "${HOST_TESTS_SRC}" -B "${BUILD_DIR}" \
    -DBROOK_SANITIZE_THREAD=ON \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    >/dev/null

cmake --build "${BUILD_DIR}" -j"$(nproc)"

# Fail loudly on any race; second-deep stacks help triage lock-order issues.
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 ${TSAN_OPTIONS:-}"

LOG_FILE="${BUILD_DIR}/tsan-ctest.log"
set +e
ctest --test-dir "${BUILD_DIR}" --output-on-failure 2>&1 | tee "${LOG_FILE}"
rc=${PIPESTATUS[0]}
set -e

if [ "${rc}" -ne 0 ]; then
    if grep -q "tsan_platform_linux.cpp.*personality" "${LOG_FILE}"; then
        echo
        echo "############################################################"
        echo "# TSan could NOT execute in this environment."
        echo "# The personality(2) syscall is blocked (seccomp ENOSYS), so"
        echo "# TSan cannot disable ASLR at startup. This is an environment"
        echo "# limitation, NOT a test failure. Run on a normal host/CI."
        echo "############################################################"
        exit 2
    fi
    echo "==> TSan reported failures (see output above)."
    exit "${rc}"
fi

echo "==> All host tests passed clean under ThreadSanitizer."
