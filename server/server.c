#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

#define MAX_CLIENTS 15
#define MAX_ROOMS 10
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define MAX_FILE_SIZE 3145728  // 3MB
#define MAX_UPLOAD_QUEUE 5
#define BUFFER_SIZE 4096

// Structures
typedef struct {
    int socket;
    char username[MAX_USERNAME_LEN + 1];
    char current_room[MAX_ROOM_NAME_LEN + 1];
    struct sockaddr_in addr;
    int active;
} Client;

typedef struct {
    char name[MAX_ROOM_NAME_LEN + 1];
    Client* members[MAX_CLIENTS];
    int member_count;
    int active;
} Room;

typedef struct {
    char filename[256];
    char sender[MAX_USERNAME_LEN + 1];
    char receiver[MAX_USERNAME_LEN + 1];
    size_t file_size;
    char* file_data;
    time_t timestamp;
} FileTransfer;

typedef struct {
    FileTransfer queue[MAX_UPLOAD_QUEUE];
    int front, rear, count;
    pthread_mutex_t mutex;
    sem_t slots;
    sem_t items;
} UploadQueue;

// Global variables
Client clients[MAX_CLIENTS];
Room rooms[MAX_ROOMS];
UploadQueue upload_queue;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_socket;
int server_running = 1;
FILE* log_file;

// Function prototypes
void* client_handler(void* arg);
void* file_transfer_handler(void* arg);
void log_message(const char* format, ...);
void send_to_client(int socket, const char* message);
void broadcast_to_room(const char* room_name, const char* message, const char* sender);
void handle_join_room(Client* client, const char* room_name);
void handle_leave_room(Client* client);
void handle_whisper(Client* client, const char* target, const char* message);
void handle_broadcast(Client* client, const char* message);
void handle_file_send(Client* client, const char* filename, const char* target);
void cleanup_client(Client* client);
void signal_handler(int sig);
int validate_username(const char* username);
int validate_room_name(const char* room_name);
int validate_filename(const char* filename);
Client* find_client_by_username(const char* username);
Room* find_or_create_room(const char* room_name);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 10000) {
        fprintf(stderr, "Invalid port number\n");
        exit(1);
    }

    // Initialize log file
    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(1);
    }

    // Initialize upload queue
    upload_queue.front = upload_queue.rear = upload_queue.count = 0;
    pthread_mutex_init(&upload_queue.mutex, NULL);
    sem_init(&upload_queue.slots, 0, MAX_UPLOAD_QUEUE);
    sem_init(&upload_queue.items, 0, 0);

    // Initialize clients and rooms
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
        clients[i].socket = -1;
    }
    for (int i = 0; i < MAX_ROOMS; i++) {
        rooms[i].active = 0;
        rooms[i].member_count = 0;
    }

    // Set up signal handler
    signal(SIGINT, signal_handler);

    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_socket);
        exit(1);
    }

    if (listen(server_socket, MAX_CLIENTS) == -1) {
        perror("Listen failed");
        close(server_socket);
        exit(1);
    }

    log_message("[SERVER] Chat server started on port %d", port);
    printf("[INFO] Server listening on port %d...\n", port);

    // Start file transfer handler thread
    pthread_t file_thread;
    pthread_create(&file_thread, NULL, file_transfer_handler, NULL);

    // Accept client connections
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket == -1) {
            if (server_running) {
                perror("Accept failed");
            }
            continue;
        }

        // Find available client slot
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                slot = i;
                break;
            }
        }

        if (slot == -1) {
            pthread_mutex_unlock(&clients_mutex);
            send_to_client(client_socket, "[ERROR] Server full. Try again later.\n");
            close(client_socket);
            continue;
        }

        // Initialize client
        clients[slot].socket = client_socket;
        clients[slot].addr = client_addr;
        clients[slot].active = 1;
        clients[slot].current_room[0] = '\0';
        clients[slot].username[0] = '\0'; // Initialize username as empty
        pthread_mutex_unlock(&clients_mutex);

        // Create client handler thread
        pthread_t client_thread;
        pthread_create(&client_thread, NULL, client_handler, &clients[slot]);
        pthread_detach(client_thread);
    }

    return 0;
}

void* client_handler(void* arg) {
    Client* client = (Client*)arg;
    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME_LEN + 1];

    // Get client IP
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    // Username registration loop - keep trying until valid unique username
    while (server_running && client->active) {
        // Request username
        send_to_client(client->socket, "Enter username (max 16 chars, alphanumeric): ");
        
        int bytes = recv(client->socket, username, MAX_USERNAME_LEN, 0);
        if (bytes <= 0) {
            cleanup_client(client);
            return NULL;
        }
        username[bytes] = '\0';
        
        // Remove newline
        char* newline = strchr(username, '\n');
        if (newline) *newline = '\0';

        // Validate username format
        if (!validate_username(username)) {
            send_to_client(client->socket, "[ERROR] Invalid username. Use alphanumeric characters only.\n");
            continue; // Try again instead of disconnecting
        }

        // Check for duplicate username
        pthread_mutex_lock(&clients_mutex);
        if (find_client_by_username(username)) {
            pthread_mutex_unlock(&clients_mutex);
            send_to_client(client->socket, "[ERROR] Username already taken. Choose another.\n");
            log_message("[REJECTED] Duplicate username attempted: %s", username);
            continue; // Try again instead of disconnecting
        }

        // Username is valid and unique, register it
        strcpy(client->username, username);
        pthread_mutex_unlock(&clients_mutex);
        break; // Exit the username registration loop
    }

    // If we got here but client is not active, it means server is shutting down or client disconnected
    if (!client->active || !server_running) {
        cleanup_client(client);
        return NULL;
    }

    log_message("[LOGIN] user '%s' connected from %s", username, client_ip);
    printf("[CONNECT] New client connected: %s from %s\n", username, client_ip); 
    send_to_client(client->socket, "[SUCCESS] Connected to chat server!\n");
    send_to_client(client->socket, "Commands: /join <room>, /leave, /broadcast <msg>, /whisper <user> <msg>, /sendfile <file> <user>, /exit\n");

    // Main command loop
    while (server_running && client->active) {
        int bytes = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            break;
        }
        buffer[bytes] = '\0';

        // Remove newline
        char* newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';

        if (strlen(buffer) == 0) continue;

        // Parse commands
        if (strncmp(buffer, "/join ", 6) == 0) {
            char room_name[MAX_ROOM_NAME_LEN + 1];
            sscanf(buffer + 6, "%32s", room_name);
            handle_join_room(client, room_name);
        }
        else if (strcmp(buffer, "/leave") == 0) {
            handle_leave_room(client);
        }
        else if (strncmp(buffer, "/broadcast ", 11) == 0) {
            handle_broadcast(client, buffer + 11);
        }
        else if (strncmp(buffer, "/whisper ", 9) == 0) {
            char target[MAX_USERNAME_LEN + 1];
            char* message = strchr(buffer + 9, ' ');
            if (message) {
                *message = '\0';
                message++;
                strcpy(target, buffer + 9);
                handle_whisper(client, target, message);
            } else {
                send_to_client(client->socket, "[ERROR] Usage: /whisper <username> <message>\n");
            }
        }
        else if (strncmp(buffer, "/sendfile ", 10) == 0) {
            char filename[256], target[MAX_USERNAME_LEN + 1];
            if (sscanf(buffer + 10, "%255s %16s", filename, target) == 2) {
                handle_file_send(client, filename, target);
            } else {
                send_to_client(client->socket, "[ERROR] Usage: /sendfile <filename> <username>\n");
            }
        }
        else if (strcmp(buffer, "/exit") == 0) {
            send_to_client(client->socket, "[INFO] Goodbye!\n");
            break;
        }
        else {
            send_to_client(client->socket, "[ERROR] Unknown command. Type a valid command.\n");
        }
    }

    cleanup_client(client);
    return NULL;
}

void* file_transfer_handler(void* arg) {
    (void)arg; 
    while (server_running) {
        sem_wait(&upload_queue.items);
        
        if (!server_running) break;

        pthread_mutex_lock(&upload_queue.mutex);
        FileTransfer transfer = upload_queue.queue[upload_queue.front];
        upload_queue.front = (upload_queue.front + 1) % MAX_UPLOAD_QUEUE;
        upload_queue.count--;
        pthread_mutex_unlock(&upload_queue.mutex);

        // Simulate file processing time
        sleep(2);

        // Find receiver
        Client* receiver = find_client_by_username(transfer.receiver);
        if (receiver && receiver->active) {
            char notification[512];
            snprintf(notification, sizeof(notification), 
                "[FILE] Received '%s' from %s (%zu bytes)\n", 
                transfer.filename, transfer.sender, transfer.file_size);
            send_to_client(receiver->socket, notification);
            
            log_message("[SEND FILE] '%s' sent from %s to %s (success)", 
                transfer.filename, transfer.sender, transfer.receiver);
        } else {
            log_message("[SEND FILE] '%s' from %s to %s (failed - user offline)", 
                transfer.filename, transfer.sender, transfer.receiver);
        }

        if (transfer.file_data) {
            free(transfer.file_data);
        }

        sem_post(&upload_queue.slots);
    }
    return NULL;
}

void log_message(const char* format, ...) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    fprintf(log_file, "%s - ", timestamp);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fflush(log_file);
    
    pthread_mutex_unlock(&log_mutex);
}

void send_to_client(int socket, const char* message) {
    send(socket, message, strlen(message), 0);
}

void broadcast_to_room(const char* room_name, const char* message, const char* sender) {
    pthread_mutex_lock(&rooms_mutex);
    
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && strcmp(rooms[i].name, room_name) == 0) {
            for (int j = 0; j < rooms[i].member_count; j++) {
                if (rooms[i].members[j] && rooms[i].members[j]->active &&
                    strcmp(rooms[i].members[j]->username, sender) != 0) {
                    char formatted_msg[BUFFER_SIZE];
                    snprintf(formatted_msg, sizeof(formatted_msg), "[%s] %s: %s\n", room_name, sender, message);
                    send_to_client(rooms[i].members[j]->socket, formatted_msg);
                }
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&rooms_mutex);
}

void handle_join_room(Client* client, const char* room_name) {
    if (!validate_room_name(room_name)) {
        send_to_client(client->socket, "[ERROR] Invalid room name. Use alphanumeric characters only.\n");
        return;
    }

    // Leave current room if any
    if (strlen(client->current_room) > 0) {
        handle_leave_room(client);
    }

    Room* room = find_or_create_room(room_name);
    if (!room) {
        send_to_client(client->socket, "[ERROR] Unable to join room.\n");
        return;
    }

    pthread_mutex_lock(&rooms_mutex);
    if (room->member_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&rooms_mutex);
        send_to_client(client->socket, "[ERROR] Room is full.\n");
        return;
    }

    room->members[room->member_count++] = client;
    strcpy(client->current_room, room_name);
    pthread_mutex_unlock(&rooms_mutex);

    char msg[256];
    snprintf(msg, sizeof(msg), "[SUCCESS] Joined room '%s'\n", room_name);
    send_to_client(client->socket, msg);
    
    log_message("[JOIN] user '%s' joined room '%s'", client->username, room_name);
    printf("[COMMAND] %s joined room '%s'\n", client->username, room_name); 
}

void handle_leave_room(Client* client) {
    if (strlen(client->current_room) == 0) {
        send_to_client(client->socket, "[ERROR] You are not in any room.\n");
        return;
    }

    pthread_mutex_lock(&rooms_mutex);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && strcmp(rooms[i].name, client->current_room) == 0) {
            // Remove client from room
            for (int j = 0; j < rooms[i].member_count; j++) {
                if (rooms[i].members[j] == client) {
                    for (int k = j; k < rooms[i].member_count - 1; k++) {
                        rooms[i].members[k] = rooms[i].members[k + 1];
                    }
                    rooms[i].member_count--;
                    break;
                }
            }
            
            // Deactivate room if empty
            if (rooms[i].member_count == 0) {
                rooms[i].active = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&rooms_mutex);

    char msg[256];
    snprintf(msg, sizeof(msg), "[SUCCESS] Left room '%s'\n", client->current_room);
    send_to_client(client->socket, msg);
    
    log_message("[LEAVE] user '%s' left room '%s'", client->username, client->current_room);
    client->current_room[0] = '\0';
}

void handle_whisper(Client* client, const char* target, const char* message) {
    Client* target_client = find_client_by_username(target);
    if (!target_client || !target_client->active) {
        send_to_client(client->socket, "[ERROR] User not found or offline.\n");
        return;
    }

    char whisper_msg[BUFFER_SIZE];
    snprintf(whisper_msg, sizeof(whisper_msg), "[WHISPER from %s]: %s\n", client->username, message);
    send_to_client(target_client->socket, whisper_msg);
    
    send_to_client(client->socket, "[SUCCESS] Whisper sent.\n");
    log_message("[WHISPER] %s to %s: %s", client->username, target, message);
    printf("[COMMAND] %s sent whisper to %s\n", client->username, target); 
}

void handle_broadcast(Client* client, const char* message) {
    if (strlen(client->current_room) == 0) {
        send_to_client(client->socket, "[ERROR] Join a room first.\n");
        return;
    }

    broadcast_to_room(client->current_room, message, client->username);
    send_to_client(client->socket, "[SUCCESS] Message broadcasted.\n");
    log_message("[BROADCAST] user '%s': %s", client->username, message);
    printf("[COMMAND] %s broadcasted to '%s'\n", client->username, client->current_room);
}

void handle_file_send(Client* client, const char* filename, const char* target) {
    if (!validate_filename(filename)) {
        send_to_client(client->socket, "[ERROR] Invalid file type. Allowed: .txt, .pdf, .jpg, .png\n");
        return;
    }

    Client* target_client = find_client_by_username(target);
    if (!target_client || !target_client->active) {
        send_to_client(client->socket, "[ERROR] Target user not found or offline.\n");
        return;
    }

    // Check file size (simulated)
    struct stat st;
    if (stat(filename, &st) == 0) {
        if (st.st_size > MAX_FILE_SIZE) {
            send_to_client(client->socket, "[ERROR] File exceeds size limit (3MB).\n");
            log_message("[ERROR] File '%s' from user '%s' exceeds size limit", filename, client->username);
            return;
        }
    }

    // Try to add to upload queue
    if (sem_trywait(&upload_queue.slots) == 0) {
        pthread_mutex_lock(&upload_queue.mutex);
        
        FileTransfer* transfer = &upload_queue.queue[upload_queue.rear];
        strcpy(transfer->filename, filename);
        strcpy(transfer->sender, client->username);
        strcpy(transfer->receiver, target);
        transfer->file_size = (st.st_mode & S_IFREG) ? st.st_size : 1024; // Default size if can't stat
        transfer->file_data = NULL; // Simplified - would contain actual file data
        transfer->timestamp = time(NULL);
        
        upload_queue.rear = (upload_queue.rear + 1) % MAX_UPLOAD_QUEUE;
        upload_queue.count++;
        
        pthread_mutex_unlock(&upload_queue.mutex);
        sem_post(&upload_queue.items);
        
        send_to_client(client->socket, "[SUCCESS] File added to upload queue.\n");
        log_message("[FILE-QUEUE] Upload '%s' from %s added to queue. Queue size: %d", 
            filename, client->username, upload_queue.count);
        printf("[COMMAND] %s initiated file transfer to %s\n", client->username, target);
    } else {
        send_to_client(client->socket, "[INFO] Upload queue full. Waiting...\n");
        
        sem_wait(&upload_queue.slots);
        
        pthread_mutex_lock(&upload_queue.mutex);
        
        FileTransfer* transfer = &upload_queue.queue[upload_queue.rear];
        strcpy(transfer->filename, filename);
        strcpy(transfer->sender, client->username);
        strcpy(transfer->receiver, target);
        transfer->file_size = (st.st_mode & S_IFREG) ? st.st_size : 1024;
        transfer->file_data = NULL;
        transfer->timestamp = time(NULL);
        
        upload_queue.rear = (upload_queue.rear + 1) % MAX_UPLOAD_QUEUE;
        upload_queue.count++;
        
        pthread_mutex_unlock(&upload_queue.mutex);
        sem_post(&upload_queue.items);
        
        send_to_client(client->socket, "[SUCCESS] File queued for upload.\n");
        log_message("[FILE-QUEUE] Upload '%s' from %s added to queue after wait. Queue size: %d", 
            filename, client->username, upload_queue.count);
    }
}

void cleanup_client(Client* client) {
    if (!client->active) return;

    // Leave current room
    if (strlen(client->current_room) > 0) {
        handle_leave_room(client);
    }

    // Log disconnection
    if (strlen(client->username) > 0) {
        log_message("[DISCONNECT] user '%s' lost connection. Cleaned up resources.", client->username);
        printf("[DISCONNECT] Client %s disconnected.\n", client->username);
    }

    // Close socket and mark inactive
    if (client->socket != -1) {
        close(client->socket);
        client->socket = -1;
    }
    
    client->active = 0;
    client->username[0] = '\0';
    client->current_room[0] = '\0';
}

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\n[SHUTDOWN] SIGINT received. Shutting down server...\n");
        server_running = 0;
        
        // Count active clients
        int active_count = 0;
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                send_to_client(clients[i].socket, "[SERVER] Server shutting down. Goodbye!\n");
                active_count++;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        
        log_message("[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs.", active_count);
        
        // Clean up
        close(server_socket);
        fclose(log_file);
        exit(0);
    }
}

int validate_username(const char* username) {
    if (!username || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return 0;
    }
    
    for (int i = 0; username[i]; i++) {
        if (!((username[i] >= 'a' && username[i] <= 'z') ||
              (username[i] >= 'A' && username[i] <= 'Z') ||
              (username[i] >= '0' && username[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

int validate_room_name(const char* room_name) {
    if (!room_name || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN) {
        return 0;
    }
    
    for (int i = 0; room_name[i]; i++) {
        if (!((room_name[i] >= 'a' && room_name[i] <= 'z') ||
              (room_name[i] >= 'A' && room_name[i] <= 'Z') ||
              (room_name[i] >= '0' && room_name[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

int validate_filename(const char* filename) {
    if (!filename) return 0;
    
    int len = strlen(filename);
    if (len < 5) return 0; // At least x.ext
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    
    return (strcmp(ext, ".txt") == 0 || strcmp(ext, ".pdf") == 0 ||
            strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0);
}

Client* find_client_by_username(const char* username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].username, username) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

Room* find_or_create_room(const char* room_name) {
    // First, try to find existing room
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && strcmp(rooms[i].name, room_name) == 0) {
            return &rooms[i];
        }
    }
    
    // Create new room
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].active) {
            strcpy(rooms[i].name, room_name);
            rooms[i].active = 1;
            rooms[i].member_count = 0;
            return &rooms[i];
        }
    }
    
    return NULL; // No available room slots
}