#pragma once
#include <string>
#include <vector>
#include <cstdint>

static constexpr const char* GLA_SOCKET_PATH = "/tmp/gla-injector.sock";

// Send a length-prefixed message (4-byte LE length + payload) on fd.
// Returns false on error.
bool glaSendMessage(int fd, const std::vector<uint8_t>& payload);

// Receive a complete length-prefixed message. Blocks until full message arrives or error.
// Returns false on EOF/error.
bool glaRecvMessage(int fd, std::vector<uint8_t>& out);

// Create a non-blocking UNIX domain server socket. Returns fd or -1.
int glaCreateServer(const std::string& path);

// Accept a client on a server fd (non-blocking). Returns client fd or -1.
int glaAcceptClient(int serverFd);

// Connect to a UNIX domain socket. Returns fd or -1.
int glaConnect(const std::string& path);
