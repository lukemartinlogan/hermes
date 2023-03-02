#!/bin/bash
CMAKE_SOURCE_DIR=$1
CMAKE_BINARY_DIR=$2
EXEC_NAME=$3
TAG_NAME=$4
TAGS=$5
MODE=$6
DO_PATH_EXCLUDE=$7
SLEEP_TIME=3

export HERMES_CONF="${CMAKE_SOURCE_DIR}/test/data/hermes_server.yaml"
export HERMES_CLIENT_CONF="${CMAKE_SOURCE_DIR}/test/data/hermes_client.yaml"

# Start the Hermes daemon
echo "STARTING DAEMON"
${CMAKE_BINARY_DIR}/bin/hermes_daemon &
echo "WAITING FOR DAEMON"
sleep ${SLEEP_TIME}

# Run the program
echo "RUNNING PROGRAM"
export LSAN_OPTIONS=suppressions="${CMAKE_SOURCE_DIR}/test/data/asan.supp"
export ADAPTER_MODE="${MODE}"
export SET_PATH="${DO_PATH_EXCLUDE}"
export COMMAND="${CMAKE_BINARY_DIR}/bin/${EXEC_NAME}"
"${COMMAND}" "${TAGS}" --reporter compact -d yes
status=$?

# Finalize the Hermes daemon
${CMAKE_BINARY_DIR}/bin/finalize_hermes

# Exit with status
exit $status