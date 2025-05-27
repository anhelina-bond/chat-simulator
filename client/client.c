// run: $ ./chatserver <port>
// $ ./chatclient <server_ip> <port>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#define BUFFER_SIZE 4096
#define MAX_INPUT_LEN 1024

// ANSI Color codes
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_RESET   "\x1b[0m"

int client_socket;
int running = 1;

void* receive_handler(void* arg);
void signal_handler(int sig);
void print_colored_message(const char* message);
void print_menu();

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(1);
    }

    const char* server_ip = argv[1];
    int port = atoi(argv[2]);

    if (port <= 0 || port > 10000) {
        fprintf(stderr, "Invalid port number\n");
        exit(1);
    }

    // Set up signal handler
    signal(SIGINT, signal_handler);

    // Create socket
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        perror("Socket creation failed");
        exit(1);
    }

    // Set up server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address\n");
        close(client_socket);
        exit(1);
    }

    // Connect to server
    printf("Connecting to server %s:%d...\n", server_ip, port);
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection failed");
        close(client_socket);
        exit(1);
    }

    printf(COLOR_GREEN "Connected to chat server!\n" COLOR_RESET);
    print_menu();

    // Create receive thread
    pthread_t receive_thread;
    pthread_create(&receive_thread, NULL, receive_handler, NULL);

    // Main input loop
    char input[MAX_INPUT_LEN];
    while (running) {
        printf("> ");
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        // Remove trailing newline
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) {
            continue;
        }

        // Check for exit command
        if (strcmp(input, "/exit") == 0) {
            running = 0;
        }

        // Send command to server
        if (send(client_socket, input, strlen(input), 0) == -1) {
            perror("Send failed");
            break;
        }

        // Add newline for server processing
        send(client_socket, "\n", 1, 0);

        if (!running) break;
    }

    // Clean up
    pthread_cancel(receive_thread);
    close(client_socket);
    printf(COLOR_YELLOW "Disconnected from server.\n" COLOR_RESET);
    return 0;
}

void* receive_handler(void* arg) {
    char buffer[BUFFER_SIZE];
    int bytes;

    while (running) {
        bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            if (running) {
                printf(COLOR_RED "\nConnection lost.\n" COLOR_RESET);
                running = 0;
            }
            break;
        }

        buffer[bytes] = '\0';
        print_colored_message(buffer);
    }

    return NULL;
}

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf(COLOR_YELLOW "\nExiting...\n" COLOR_RESET);
        running = 0;
        close(client_socket);
        exit(0);
    }
}

void print_colored_message(const char* message) {
    // Color code messages based on type
    if (strstr(message, "[ERROR]")) {
        printf(COLOR_RED "%s" COLOR_RESET, message);
    }
    else if (strstr(message, "[SUCCESS]")) {
        printf(COLOR_GREEN "%s" COLOR_RESET, message);
    }
    else if (strstr(message, "[INFO]")) {
        printf(COLOR_BLUE "%s" COLOR_RESET, message);
    }
    else if (strstr(message, "[WHISPER")) {
        printf(COLOR_MAGENTA "%s" COLOR_RESET, message);
    }
    else if (strstr(message, "[FILE]")) {
        printf(COLOR_CYAN "%s" COLOR_RESET, message);
    }
    else if (strstr(message, "[SERVER]")) {
        printf(COLOR_YELLOW "%s" COLOR_RESET, message);
    }
    else {
        printf("%s", message);
    }
    
    // Print prompt again if message doesn't end with newline
    if (message[strlen(message) - 1] != '\n') {
        printf("\n> ");
        fflush(stdout);
    }
}

void print_menu() {
    printf(COLOR_CYAN "\n=== Chat Client Commands ===\n" COLOR_RESET);
    printf("/join <room_name>     - Join or create a room\n");
    printf("/leave               - Leave current room\n");
    printf("/broadcast <message> - Send message to room\n");
    printf("/whisper <user> <msg>- Send private message\n");
    printf("/sendfile <file> <user> - Send file to user\n");
    printf("/exit                - Disconnect from server\n");
    printf(COLOR_CYAN "============================\n\n" COLOR_RESET);
}