/*
 * tcp_server.cpp
 *
 * TODO: add epoll/kqueue for better scalability. Right now it's
 * one-thread-per-connection via the pool, which is fine for the
 * benchmark but wouldn't hold up under thousands of clients.
 */

#include "server/network/tcp_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

namespace flexql {

// ---------------------------------------------------------------------------
//  Constructor / Destructor
// ---------------------------------------------------------------------------

TCPServer::TCPServer(Executor& executor, uint16_t port, size_t num_threads)
    : executor_(executor), port_(port), pool_(num_threads) {}

TCPServer::~TCPServer() {
    stop();
}

// ---------------------------------------------------------------------------
//  Run
// ---------------------------------------------------------------------------

void TCPServer::run() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[FlexQL] socket() failed: " << strerror(errno) << "\n";
        return;
    }

    // SO_REUSEADDR + SO_REUSEPORT — eliminates "Address already in use"
    int opt = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[FlexQL] bind() failed: " << strerror(errno) << "\n";
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (::listen(server_fd_, 128) < 0) {
        std::cerr << "[FlexQL] listen() failed: " << strerror(errno) << "\n";
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    running_.store(true);
    std::cout << "[FlexQL] Server listening on port " << port_ << "\n";

    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!running_.load()) break;  // shutdown
            if (errno == EINTR) continue;
            std::cerr << "[FlexQL] accept() failed: " << strerror(errno) << "\n";
            continue;
        }

        // TCP_NODELAY for low latency (L-26)
        int nodelay = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Dispatch to thread pool
        pool_.submit([this, client_fd]() {
            handle_client(client_fd);
        });
    }
}

// ---------------------------------------------------------------------------
//  Stop
// ---------------------------------------------------------------------------

void TCPServer::stop() {
    running_.store(false);
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }
    pool_.shutdown();
}

// ---------------------------------------------------------------------------
//  Handle Client (one connection, potentially multiple queries)
// ---------------------------------------------------------------------------

void TCPServer::handle_client(int client_fd) {
    std::string query;
    while (recv_message(client_fd, query)) {
        // Trim
        while (!query.empty() && (query.back() == '\n' || query.back() == '\r' || query.back() == ';'))
            query.pop_back();

        if (query.empty()) continue;

        // Check for EXIT/QUIT
        {
            std::string upper = query;
            for (auto& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (upper == "EXIT" || upper == "QUIT") {
                send_message(client_fd, "BYE\n");
                break;
            }
        }

        auto result = executor_.execute_sql(query);
        std::string response = format_result(result);
        if (!send_message(client_fd, response)) break;
    }
    ::close(client_fd);
}

// ---------------------------------------------------------------------------
//  Wire Protocol: Length-Prefixed Messages
// ---------------------------------------------------------------------------

bool TCPServer::send_message(int fd, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    // Send length header
    ssize_t sent = 0;
    const char* ptr = reinterpret_cast<const char*>(&len);
    while (sent < 4) {
        ssize_t n = ::send(fd, ptr + sent, 4 - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    // Send payload
    sent = 0;
    ptr = msg.data();
    while (sent < static_cast<ssize_t>(msg.size())) {
        ssize_t n = ::send(fd, ptr + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool TCPServer::recv_message(int fd, std::string& msg) {
    // Read 4-byte length header
    uint32_t net_len = 0;
    ssize_t rcvd = 0;
    char* ptr = reinterpret_cast<char*>(&net_len);
    while (rcvd < 4) {
        ssize_t n = ::recv(fd, ptr + rcvd, 4 - rcvd, 0);
        if (n <= 0) return false;
        rcvd += n;
    }
    uint32_t len = ntohl(net_len);
    if (len > 10 * 1024 * 1024) return false;  // 10MB max message

    msg.resize(len);
    rcvd = 0;
    while (rcvd < static_cast<ssize_t>(len)) {
        ssize_t n = ::recv(fd, &msg[rcvd], len - rcvd, 0);
        if (n <= 0) return false;
        rcvd += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Format Result
// ---------------------------------------------------------------------------

std::string TCPServer::format_result(const Executor::QueryResult& result) {
    std::ostringstream oss;
    if (!result.success) {
        oss << "ERROR: " << result.error << "\n";
    } else if (!result.column_names.empty()) {
        // SELECT result: header + rows
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (i > 0) oss << "\t";
            oss << result.column_names[i];
        }
        oss << "\n";
        for (const auto& row : result.rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) oss << "\t";
                oss << row[i].to_string();
            }
            oss << "\n";
        }
    } else {
        oss << result.message << "\n";
    }
    return oss.str();
}

}  // namespace flexql
