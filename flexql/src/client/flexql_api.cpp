/*
 * flexql_api.cpp — client-side API implementation
 *
 * Implements the public C API (flexql_open, exec, close, free).
 * Talks to the server over TCP with length-prefixed messages.
 */

#include "flexql.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Internal handle structure
// ---------------------------------------------------------------------------

struct FlexQL {
    int         fd;
    std::string host;
    int         port;
    std::string last_error;
};

// ---------------------------------------------------------------------------
//  Wire helpers (same protocol as server)
// ---------------------------------------------------------------------------

static bool wire_send(int fd, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    ssize_t sent = 0;
    const char* ptr = reinterpret_cast<const char*>(&len);
    while (sent < 4) {
        ssize_t n = ::send(fd, ptr + sent, 4 - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    sent = 0;
    ptr = msg.data();
    while (sent < static_cast<ssize_t>(msg.size())) {
        ssize_t n = ::send(fd, ptr + sent, msg.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool wire_recv(int fd, std::string& msg) {
    uint32_t net_len = 0;
    ssize_t rcvd = 0;
    char* ptr = reinterpret_cast<char*>(&net_len);
    while (rcvd < 4) {
        ssize_t n = ::recv(fd, ptr + rcvd, 4 - rcvd, 0);
        if (n <= 0) return false;
        rcvd += n;
    }
    uint32_t len = ntohl(net_len);
    if (len > 10 * 1024 * 1024) return false;

    msg.resize(len);
    rcvd = 0;
    while (rcvd < static_cast<ssize_t>(len)) {
        ssize_t n = ::recv(fd, &msg[rcvd], len - rcvd, 0);
        if (n <= 0) return false;
        rcvd += n;
    }
    return true;
}

static char* strdup_c(const std::string& s) {
    char* p = static_cast<char*>(::malloc(s.size() + 1));
    if (p) {
        ::memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

// ---------------------------------------------------------------------------
//  Public C API
// ---------------------------------------------------------------------------

extern "C" {

int flexql_open(const char* host, int port, FlexQL** db) {
    if (!host || !db) return FLEXQL_MISUSE;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return FLEXQL_ERROR;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        ::close(fd);
        return FLEXQL_ERROR;
    }

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return FLEXQL_ERROR;
    }

    // TCP_NODELAY
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    FlexQL* handle = new FlexQL();
    handle->fd   = fd;
    handle->host = host;
    handle->port = port;
    *db          = handle;

    return FLEXQL_OK;
}

int flexql_close(FlexQL* db) {
    if (!db) return FLEXQL_MISUSE;
    if (db->fd >= 0) {
        wire_send(db->fd, "EXIT");
        std::string response;
        wire_recv(db->fd, response);
        ::close(db->fd);
        db->fd = -1;
    }
    delete db;
    return FLEXQL_OK;
}

int flexql_exec(FlexQL* db, const char* sql,
                flexql_callback callback, void* arg, char** errmsg) {
    if (!db || !sql) return FLEXQL_MISUSE;
    if (db->fd < 0) return FLEXQL_ERROR;

    if (errmsg) *errmsg = nullptr;

    if (!wire_send(db->fd, std::string(sql))) {
        if (errmsg) *errmsg = strdup_c("Failed to send query");
        return FLEXQL_ERROR;
    }

    std::string response;
    if (!wire_recv(db->fd, response)) {
        if (errmsg) *errmsg = strdup_c("Failed to receive response");
        return FLEXQL_ERROR;
    }

    // Check for error
    if (response.size() >= 7 && response.substr(0, 7) == "ERROR: ") {
        if (errmsg) *errmsg = strdup_c(response.substr(7));
        return FLEXQL_ERROR;
    }

    // Parse response and invoke callback
    if (callback) {
        // Split into lines
        std::vector<std::string> lines;
        {
            size_t start = 0;
            while (start < response.size()) {
                size_t end = response.find('\n', start);
                if (end == std::string::npos) {
                    if (start < response.size())
                        lines.push_back(response.substr(start));
                    break;
                }
                lines.push_back(response.substr(start, end - start));
                start = end + 1;
            }
        }

        if (lines.size() >= 2) {
            // First line = column names, rest = data rows
            std::vector<std::string> col_names;
            {
                const auto& hdr = lines[0];
                size_t start = 0;
                while (start < hdr.size()) {
                    size_t tab = hdr.find('\t', start);
                    if (tab == std::string::npos) {
                        col_names.push_back(hdr.substr(start));
                        break;
                    }
                    col_names.push_back(hdr.substr(start, tab - start));
                    start = tab + 1;
                }
            }
            int ncols = static_cast<int>(col_names.size());

            std::vector<char*> col_ptrs(static_cast<size_t>(ncols));
            for (int i = 0; i < ncols; ++i) col_ptrs[i] = &col_names[i][0];

            for (size_t r = 1; r < lines.size(); ++r) {
                if (lines[r].empty()) continue;
                std::vector<std::string> vals;
                {
                    const auto& row_str = lines[r];
                    size_t start = 0;
                    while (start < row_str.size()) {
                        size_t tab = row_str.find('\t', start);
                        if (tab == std::string::npos) {
                            vals.push_back(row_str.substr(start));
                            break;
                        }
                        vals.push_back(row_str.substr(start, tab - start));
                        start = tab + 1;
                    }
                }
                while (static_cast<int>(vals.size()) < ncols) vals.emplace_back("");

                std::vector<char*> val_ptrs(static_cast<size_t>(ncols));
                for (int i = 0; i < ncols; ++i) val_ptrs[i] = &vals[i][0];

                int rc = callback(arg, ncols, val_ptrs.data(), col_ptrs.data());
                if (rc != 0) break;
            }
        }
    }

    return FLEXQL_OK;
}

void flexql_free(void* ptr) {
    ::free(ptr);
}

}  // extern "C"
