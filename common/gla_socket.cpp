#include "gla_socket.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

bool glaSendMessage(int fd, const std::vector<uint8_t>& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    if (write(fd, &len, 4) != 4) return false;
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = write(fd, payload.data() + sent, payload.size() - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool glaRecvMessage(int fd, std::vector<uint8_t>& out) {
    uint32_t len = 0;
    size_t got = 0;
    while (got < 4) {
        ssize_t n = read(fd, reinterpret_cast<uint8_t*>(&len) + got, 4 - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    out.resize(len);
    got = 0;
    while (got < len) {
        ssize_t n = read(fd, out.data() + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

int glaCreateServer(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    unlink(path.c_str());
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int glaAcceptClient(int serverFd) {
    return accept(serverFd, nullptr, nullptr);
}

int glaConnect(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}
