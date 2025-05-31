#!/bin/bash

# Test script for 30 concurrent users
# CSE 344 Final Project - Multi-threaded Chat Server Test

# Configuration
SERVER_IP="127.0.0.1"
SERVER_PORT="8080"
NUM_CLIENTS=30
CLIENT_EXECUTABLE="./chatclient"
LOG_DIR="test_logs"
PIDS_FILE="client_pids.txt"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Create log directory
mkdir -p $LOG_DIR

# Clear previous PIDs file
> $PIDS_FILE

echo -e "${BLUE}=== CSE 344 - 30 Concurrent Users Test ===${NC}"
echo -e "${YELLOW}Server IP: $SERVER_IP${NC}"
echo -e "${YELLOW}Server Port: $SERVER_PORT${NC}"
echo -e "${YELLOW}Number of clients: $NUM_CLIENTS${NC}"
echo ""

# Function to generate random username
generate_username() {
    local id=$1
    local prefixes=("user" "client" "test" "demo" "chat" "ali" "mehmet" "ayse" "emre" "irem")
    local prefix=${prefixes[$((RANDOM % ${#prefixes[@]}))]}
    echo "${prefix}${id}"
}

# Function to generate random room name
generate_room() {
    local rooms=("project1" "team1" "general" "study" "work" "chat" "meeting" "group1" "group2" "room1")
    echo ${rooms[$((RANDOM % ${#rooms[@]}))]}
}

# Function to create client script
create_client_script() {
    local client_id=$1
    local username=$(generate_username $client_id)
    local room=$(generate_room)
    local script_file="$LOG_DIR/client_${client_id}_script.txt"
    
    cat > $script_file << EOF
$username
/join $room
/broadcast Hello from $username in $room!
/broadcast Testing concurrent connections - client $client_id
/whisper user1 Private message from $username
/broadcast Another message from $username
/leave
/join general
/broadcast $username joined general room
/broadcast Final message from $username
/exit
EOF
    
    echo $script_file
}

# Function to start a single client
start_client() {
    local client_id=$1
    local script_file=$(create_client_script $client_id)
    local log_file="$LOG_DIR/client_${client_id}.log"
    
    echo -e "${BLUE}Starting client $client_id...${NC}"
    
    # Start client with input script and log output
    $CLIENT_EXECUTABLE $SERVER_IP $SERVER_PORT < $script_file > $log_file 2>&1 &
    local pid=$!
    echo $pid >> $PIDS_FILE
    
    echo -e "${GREEN}Client $client_id started (PID: $pid)${NC}"
}

# Function to check if server is running
check_server() {
    echo -e "${YELLOW}Checking if server is running on port $SERVER_PORT...${NC}"
    if ! nc -z $SERVER_IP $SERVER_PORT 2>/dev/null; then
        echo -e "${RED}ERROR: Server is not running on $SERVER_IP:$SERVER_PORT${NC}"
        echo -e "${YELLOW}Please start the server first:${NC}"
        echo -e "${YELLOW}  ./chatserver $SERVER_PORT${NC}"
        exit 1
    fi
    echo -e "${GREEN}Server is running!${NC}"
}

# Function to start all clients
start_all_clients() {
    echo -e "${BLUE}Starting $NUM_CLIENTS concurrent clients...${NC}"
    echo ""
    
    for i in $(seq 1 $NUM_CLIENTS); do
        start_client $i
        # Small delay to avoid overwhelming the server at startup
        sleep 0.1
    done
    
    echo ""
    echo -e "${GREEN}All $NUM_CLIENTS clients started!${NC}"
}

# Function to monitor clients
monitor_clients() {
    echo -e "${YELLOW}Monitoring client connections...${NC}"
    echo ""
    
    local active_clients=0
    local total_time=0
    local max_wait=60  # Maximum wait time in seconds
    
    while [ $total_time -lt $max_wait ]; do
        active_clients=0
        
        # Count active client processes
        if [ -f $PIDS_FILE ]; then
            while read pid; do
                if kill -0 $pid 2>/dev/null; then
                    ((active_clients++))
                fi
            done < $PIDS_FILE
        fi
        
        echo -e "${BLUE}Active clients: $active_clients/${NUM_CLIENTS} (Time: ${total_time}s)${NC}"
        
        if [ $active_clients -eq 0 ]; then
            echo -e "${GREEN}All clients have finished!${NC}"
            break
        fi
        
        sleep 2
        ((total_time+=2))
    done
    
    if [ $active_clients -gt 0 ]; then
        echo -e "${YELLOW}Some clients are still running after ${max_wait}s${NC}"
        echo -e "${YELLOW}Remaining active clients: $active_clients${NC}"
    fi
}

# Function to cleanup processes
cleanup() {
    echo -e "${YELLOW}Cleaning up client processes...${NC}"
    
    if [ -f $PIDS_FILE ]; then
        while read pid; do
            if kill -0 $pid 2>/dev/null; then
                kill $pid 2>/dev/null
                echo -e "${YELLOW}Terminated client PID: $pid${NC}"
            fi
        done < $PIDS_FILE
    fi
    
    # Remove PID file
    rm -f $PIDS_FILE
}

# Function to generate test report
generate_report() {
    echo -e "${BLUE}Generating test report...${NC}"
    
    local report_file="$LOG_DIR/test_report.txt"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    
    cat > $report_file << EOF
=== 30 Concurrent Users Test Report ===
Test Date: $timestamp
Server: $SERVER_IP:$SERVER_PORT
Number of Clients: $NUM_CLIENTS

=== Test Configuration ===
- Each client connects with a unique username
- Clients join different rooms randomly
- Each client sends multiple messages (broadcast and whisper)
- Clients switch between rooms
- All clients disconnect gracefully

=== Client Activities ===
Each client performed the following actions:
1. Connect with unique username
2. Join a random room
3. Send broadcast messages
4. Send whisper messages
5. Leave room and join 'general' room
6. Send more messages
7. Exit gracefully

=== Log Files Generated ===
EOF

    # List all generated log files
    ls -la $LOG_DIR/client_*.log >> $report_file 2>/dev/null
    
    echo ""
    echo -e "${GREEN}Test report generated: $report_file${NC}"
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -p, --port     Server port (default: 8080)"
    echo "  -i, --ip       Server IP (default: 127.0.0.1)"
    echo "  -n, --num      Number of clients (default: 30)"
    echo "  -c, --clean    Clean up previous test files"
    echo ""
    echo "Example:"
    echo "  $0 -p 9090 -n 25"
}

# Function to clean previous test files
clean_previous() {
    echo -e "${YELLOW}Cleaning previous test files...${NC}"
    rm -rf $LOG_DIR
    rm -f $PIDS_FILE
    echo -e "${GREEN}Previous test files cleaned!${NC}"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -p|--port)
            SERVER_PORT="$2"
            shift 2
            ;;
        -i|--ip)
            SERVER_IP="$2"
            shift 2
            ;;
        -n|--num)
            NUM_CLIENTS="$2"
            shift 2
            ;;
        -c|--clean)
            clean_previous
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            show_usage
            exit 1
            ;;
    esac
done

# Signal handlers for cleanup
trap cleanup EXIT
trap cleanup SIGINT
trap cleanup SIGTERM

# Main execution
main() {
    echo -e "${BLUE}=== Starting 30 Concurrent Users Test ===${NC}"
    echo ""
    
    # Check if client executable exists
    if [ ! -f "$CLIENT_EXECUTABLE" ]; then
        echo -e "${RED}ERROR: Client executable not found: $CLIENT_EXECUTABLE${NC}"
        echo -e "${YELLOW}Please compile the client first${NC}"
        exit 1
    fi
    
    # Check if server is running
    check_server
    
    echo ""
    echo -e "${YELLOW}Test will start in 3 seconds...${NC}"
    sleep 3
    
    # Start all clients
    start_all_clients
    
    # Monitor clients
    monitor_clients
    
    # Generate report
    generate_report
    
    echo ""
    echo -e "${GREEN}=== Test Completed Successfully! ===${NC}"
    echo -e "${YELLOW}Check the log files in: $LOG_DIR/${NC}"
    echo -e "${YELLOW}Server logs should show all client activities${NC}"
}

# Run main function
main


# # Make executable
# chmod +x test.sh

# # Basic usage (30 clients, default settings)
# ./test.sh

# # Custom configuration
# ./test.sh -p 9090 -n 25 -i 192.168.1.100

# # Clean previous test files
# ./test.sh --clean

# # Show help
# ./test.sh --help