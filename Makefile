CC = gcc
CFLAGS = -Wall -Wextra -pthread -std=c99 -D_GNU_SOURCE
SERVER_SRC = server/server.c
CLIENT_SRC = client/client.c
SERVER_TARGET = chatserver
CLIENT_TARGET = chatclient

.PHONY: all clean server client

all: server client

server: $(SERVER_TARGET)

client: $(CLIENT_TARGET)

$(SERVER_TARGET): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_TARGET) $(SERVER_SRC)

$(CLIENT_TARGET): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_TARGET) $(CLIENT_SRC)

clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET) server.log

install: all
	mkdir -p server client
	cp server.c server/
	cp client.c client/

test: all
	@echo "Testing server startup..."
	@timeout 2s ./$(SERVER_TARGET) 8080 || true
	@echo "Test completed."

.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all     - Build both server and client"
	@echo "  server  - Build server only"
	@echo "  client  - Build client only"
	@echo "  clean   - Remove executables and logs"
	@echo "  install - Create directory structure"
	@echo "  test    - Basic functionality test"
	@echo ""
	@echo "Usage:"
	@echo "  ./chatserver <port>"
	@echo "  ./chatclient <server_ip> <port>"