#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdint>
#include <string>
#include <vector>

static constexpr const char* glaSocketPath = "/tmp/gla-injector.sock";

// Send a length-prefixed message (4-byte LE length + payload) on fd.
// Returns false on error.
inline bool glaSendMessage (int fd, const std::vector<uint8_t>& payload)
{
    uint32_t len = static_cast<uint32_t> (payload.size());

    ssize_t hw;
    do { hw = write (fd, &len, 4); } while (hw < 0 && errno == EINTR);
    if (hw != 4) return false;

    size_t sent = 0;

    while (sent < payload.size())
    {
        ssize_t n = write (fd, payload.data() + sent, payload.size() - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t> (n);
    }

    return true;
}

// Receive a complete length-prefixed message. Blocks until full message arrives or error.
// Returns false on EOF/error or if the declared length exceeds the sanity limit.
static constexpr uint32_t kGLAMaxMessageLen = 1u * 1024u * 1024u; // 1 MB — no legitimate message approaches this

inline bool glaRecvMessage (int fd, std::vector<uint8_t>& out)
{
    uint32_t len = 0;
    size_t got = 0;

	while (got < 4)
    {
        auto n = read (fd, reinterpret_cast<uint8_t*> (&len) + got, 4 - got);

		if (n <= 0)
			return false;

        got += static_cast<size_t> (n);
    }

    // Reject absurdly large lengths before allocating — a malformed sender sending
    // 0xFFFFFFFF would otherwise trigger std::bad_alloc -> std::terminate in the driver.
    if (len > kGLAMaxMessageLen)
        return false;

    out.resize (len);
    got = 0;

	while (got < len)
    {
        ssize_t n = read (fd, out.data() + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t> (n);
    }

	return true;
}

// Create a non-blocking UNIX domain server socket. Returns fd or -1.
inline int glaCreateServer (const std::string& path)
{
    int fd = socket (AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

    unlink (path.c_str());
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy (addr.sun_path, path.c_str(), sizeof (addr.sun_path) - 1);

	if (bind (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) < 0)
    {
        close (fd);
        return -1;
    }

    // Allow _coreaudiod (which runs the driver) to connect — it's a different
    // user so it hits the "other" permission bits, which need write access.
    chmod (path.c_str(), 0777);

	if (listen (fd, 8) < 0)
	{
		close (fd);
		return -1;
	}

    fcntl (fd, F_SETFL, O_NONBLOCK);
    return fd;
}

// Accept a client on a server fd (non-blocking). Returns client fd or -1.
inline int glaAcceptClient (int serverFd)
{
    int fd = accept (serverFd, nullptr, nullptr);

    if (fd >= 0)
    {
        // Default Unix domain socket buffer (8KB) is smaller than a single
        // 20-channel audio frame (40KB), causing blocking writes. Bump to 256KB.
        int sz = 256 * 1024;
        setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof (sz));
        setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof (sz));
    }

    return fd;
}

// Connect to a UNIX domain socket. Returns fd or -1.
inline int glaConnect (const std::string& path)
{
    int fd = socket (AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
		return -1;

	sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy (addr.sun_path, path.c_str(), sizeof (addr.sun_path) - 1);

    if (connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) < 0)
    {
        close (fd);
        return -1;
    }

    int sz = 256 * 1024;
    setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof (sz));
    setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof (sz));

    return fd;
}
