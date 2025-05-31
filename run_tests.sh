#!/bin/bash
set -e

# Compilation chmod +x run_tests.sh

# Configuration   
SERVER_PORT=5000
SERVER_LOG="test_server.log"
CLIENT_LOG_PREFIX="test_client"
TEST_DIR="test_files"
mkdir -p $TEST_DIR

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    pkill -f "chatserver $SERVER_PORT" || true
    rm -f $SERVER_LOG
    rm -f ${CLIENT_LOG_PREFIX}*
    rm -rf $TEST_DIR
}

# Start the server
start_server() {
    echo "Starting server on port $SERVER_PORT..."
    ./chatserver $SERVER_PORT > $SERVER_LOG 2>&1 &
    SERVER_PID=$!
    sleep 2 # Wait for server to start
}

# Stop the server
stop_server() {
    echo "Stopping server..."
    kill -SIGINT $SERVER_PID
    wait $SERVER_PID
}

# Run client command
run_client() {
    local client_id=$1
    shift
    local commands=("$@")
    local log_file="${CLIENT_LOG_PREFIX}_${client_id}.log"
    
    # Create named pipe for input
    local input_pipe="${CLIENT_LOG_PREFIX}_input.pipe"
    rm -f $input_pipe
    mkfifo $input_pipe
    
    # Start client in background
    (./chatclient 127.0.0.1 $SERVER_PORT < $input_pipe > $log_file 2>&1) &
    local client_pid=$!
    
    # Feed commands to client
    (
        for cmd in "${commands[@]}"; do
            sleep 0.5
            echo "$cmd"
        done
        sleep 0.5
        echo "/exit"
    ) > $input_pipe
    
    wait $client_pid
    rm -f $input_pipe
}

# Test 1: Duplicate Usernames
test_duplicate_usernames() {
    echo "Running Test 1: Duplicate Usernames"
    
    run_client 1 "user1" "/join room1" "/broadcast Hello"
    run_client 2 "user1" ""
    
    if grep -q "Username already taken" ${CLIENT_LOG_PREFIX}_2.log; then
        echo "PASS: Duplicate username rejected"
    else
        echo "FAIL: Duplicate username not detected"
        exit 1
    fi
    
    if grep -q "REJECTED" $SERVER_LOG; then
        echo "PASS: Server logged rejection"
    else
        echo "FAIL: Server log missing rejection"
        exit 1
    fi
}

# Test 2: File Upload Queue Limit
test_file_queue() {
    echo "Running Test 2: File Upload Queue"
    
    # Create test files
    for i in {1..7}; do
        dd if=/dev/urandom of=$TEST_DIR/file$i.txt bs=1024 count=10 2>/dev/null
    done
    
    # Start clients that send files
    for i in {1..7}; do
        (
            run_client "file$i" "user$i" "/sendfile $TEST_DIR/file$i.txt user_target"
        ) &
    done
    
    sleep 10 # Wait for file transfers to process
    
    # Check queue behavior
    if grep -q "Upload queue full" ${CLIENT_LOG_PREFIX}_file*.log; then
        echo "PASS: Queue full detected"
    else
        echo "FAIL: Queue full not detected"
        exit 1
    fi
    
    if grep -q "added to queue" $SERVER_LOG; then
        echo "PASS: Files added to queue"
    else
        echo "FAIL: Queue not used"
        exit 1
    fi
}

# Test 3: Room Switching and History
test_room_switching() {
    echo "Running Test 3: Room Switching and History"
    
    run_client "room1" "room_user" \
        "/join roomA" \
        "/broadcast Message1" \
        "/leave" \
        "/join roomB" \
        "/broadcast Message2" \
        "/leave" \
        "/join roomA" \
        ""
    
    if grep -q "Message1" ${CLIENT_LOG_PREFIX}_room1.log; then
        echo "PASS: History message displayed"
    else
        echo "FAIL: History not shown"
        exit 1
    fi
    
    if ! grep -q "Message2" ${CLIENT_LOG_PREFIX}_room1.log; then
        echo "PASS: Room separation maintained"
    else
        echo "FAIL: Room history mixed"
        exit 1
    fi
}

# Test 4: Oversized File Rejection
test_oversized_file() {
    echo "Running Test 4: Oversized File Rejection"
    
    # Create 4MB file (over 3MB limit)
    dd if=/dev/zero of=$TEST_DIR/bigfile bs=1M count=4 2>/dev/null
    
    run_client "bigfile" "big_user" \
        "/sendfile $TEST_DIR/bigfile target_user"
    
    if grep -q "exceeds size limit" ${CLIENT_LOG_PREFIX}_bigfile.log; then
        echo "PASS: Oversized file rejected"
    else
        echo "FAIL: Oversized file accepted"
        exit 1
    fi
    
    if grep -q "exceeds size limit" $SERVER_LOG; then
        echo "PASS: Server logged size violation"
    else
        echo "FAIL: Size violation not logged"
        exit 1
    fi
}

# Test 5: Concurrent User Load
test_concurrent_users() {
    echo "Running Test 5: Concurrent User Load"
    
    # Start 30 clients simultaneously
    for i in {1..30}; do
        (
            run_client "concurrent$i" "user$i" \
                "/join room$i" \
                "/broadcast Hello from user$i" \
                "/exit"
        ) &
    done
    
    wait
    
    # Verify all joined and broadcasted
    join_count=$(grep -c "Joined room" $SERVER_LOG)
    broadcast_count=$(grep -c "broadcasted to" $SERVER_LOG)
    
    if [ "$join_count" -ge 30 ] && [ "$broadcast_count" -ge 30 ]; then
        echo "PASS: All users processed"
    else
        echo "FAIL: Concurrent users not handled"
        echo "Joins: $join_count, Broadcasts: $broadcast_count"
        exit 1
    fi
}

# Test 6: Unexpected Disconnection
test_unexpected_disconnect() {
    echo "Running Test 6: Unexpected Disconnection"
    
    # Start client in background
    ./chatclient 127.0.0.1 $SERVER_PORT > ${CLIENT_LOG_PREFIX}_disconnect.log 2>&1 &
    local client_pid=$!
    
    # Let it connect
    sleep 1
    
    # Kill abruptly
    kill -9 $client_pid
    
    # Wait for server to detect disconnect
    sleep 2
    
    if grep -q "DISCONNECT" $SERVER_LOG; then
        echo "PASS: Server detected disconnect"
    else
        echo "FAIL: Disconnect not detected"
        exit 1
    fi
}

# Test 7: SIGINT Server Shutdown
test_sigint_shutdown() {
    echo "Running Test 7: SIGINT Server Shutdown"
    
    start_server
    
    # Start a client
    run_client "shutdown" "shutdown_user" "/join shutdown_room" &
    sleep 1
    
    # Send SIGINT to server
    kill -SIGINT $SERVER_PID
    wait $SERVER_PID
    
    # Check shutdown process
    if grep -q "SHUTDOWN" $SERVER_LOG; then
        echo "PASS: Server shutdown initiated"
    else
        echo "FAIL: Shutdown not logged"
        exit 1
    fi
    
    if grep -q "Server shutting down" ${CLIENT_LOG_PREFIX}_shutdown.log; then
        echo "PASS: Clients notified"
    else
        echo "FAIL: Clients not notified"
        exit 1
    fi
}

# Test 8: Filename Collision
test_filename_collision() {
    echo "Running Test 8: Filename Collision"
    
    # Create test files with same name
    echo "Content1" > $TEST_DIR/duplicate.txt
    echo "Content2" > $TEST_DIR/duplicate.txt
    
    start_server
    
    # Send two files with same name
    run_client "collide1" "sender1" "/sendfile $TEST_DIR/duplicate.txt receiver" &
    run_client "collide2" "sender2" "/sendfile $TEST_DIR/duplicate.txt receiver" &
    
    sleep 5
    
    stop_server
    
    if grep -q "renamed" $SERVER_LOG; then
        echo "PASS: Filename collision resolved"
    else
        echo "FAIL: Filename collision not handled"
        exit 1
    fi
}

# Run all tests
start_server
test_duplicate_usernames
test_room_switching
test_concurrent_users
test_oversized_file
test_unexpected_disconnect
stop_server

test_file_queue
test_filename_collision
test_sigint_shutdown

echo ""
echo "========================================"
echo "All tests passed successfully!"
echo "========================================"