#!/bin/bash
# filepath: /home/alan/Documents/Repositories/embedded_linux/run_tests.sh

set -e  # Exit on error

REPO_DIR="/home/alan/Documents/Repositories/embedded_linux"
DRIVER_DIR="${REPO_DIR}/aesd-char-driver"
AUTOTEST_DIR="${REPO_DIR}/assignment-autotest/test"
TEST_USER="${SUDO_USER:-$USER}"

check_aesdchar_access() {
    local device="/dev/aesdchar"

    if [ ! -e "${device}" ]; then
        echo "ERROR: ${device} does not exist after driver load"
        exit 1
    fi

    if ! sudo -u "${TEST_USER}" test -r "${device}" -a -w "${device}"; then
        echo "ERROR: ${TEST_USER} does not have read/write access to ${device}"
        ls -l "${device}"
        echo "Hint: reload driver with sudo ./aesd-char-driver/aesdchar_load"
        exit 1
    fi
}

# Unload and reload driver
sudo "${DRIVER_DIR}/aesdchar_unload" || { echo "Failed to unload driver"; exit 1; }
sudo "${DRIVER_DIR}/aesdchar_load" || { echo "Failed to load driver"; exit 1; }
check_aesdchar_access
echo ""

# Run appropriate test
case $1 in
    "assignment6")
        sudo "${AUTOTEST_DIR}/assignment6/sockettest.sh"
        ;;
    "assignment8")
        sudo "${AUTOTEST_DIR}/assignment8/sockettest.sh"
        ;;
    "assignment9")
        sudo "${AUTOTEST_DIR}/assignment9/sockettest.sh"
        ;;
    "drivertest")
        sudo "${AUTOTEST_DIR}/assignment9/drivertest.sh"
        ;;
    *)
        echo "Usage: $0 {assignment6|assignment8|assignment9|drivertest}"
        exit 1
        ;;
esac

echo ""
cd "${REPO_DIR}"