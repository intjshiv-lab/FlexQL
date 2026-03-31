/*
 * tcp_server.h — TCP networking layer
 *
 * Accepts connections, reads SQL, dispatches to the executor, writes
 * results back. Uses a simple length-prefixed wire protocol:
 *
 *   [4 bytes: payload length, network order] [payload]
 *
 * Response is plain text — errors get "ERROR:" prefix, SELECTs
 * return tab-separated rows. Nothing fancy but it works.
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_TCP_SERVER_H
#define FLEXQL_TCP_SERVER_H

#include "server/executor/executor.h"
#include "server/concurrency/thread_pool.h"
#include <string>
#include <atomic>
#include <cstdint>

namespace flexql {

class TCPServer {
public:
    TCPServer(Executor& executor, uint16_t port, size_t num_threads);
    ~TCPServer();

    // Start listening (blocking until stop() is called)
    void run();

    // Stop the server
    void stop();

    // Is the server running?
    bool is_running() const { return running_.load(); }

private:
    void handle_client(int client_fd);

    // Wire helpers
    static bool send_message(int fd, const std::string& msg);
    static bool recv_message(int fd, std::string& msg);

    // Format query result to wire format
    static std::string format_result(const Executor::QueryResult& result);

    Executor&           executor_;
    uint16_t            port_;
    int                 server_fd_ = -1;
    std::atomic<bool>   running_{false};
    ThreadPool          pool_;
};

}  // namespace flexql

#endif /* FLEXQL_TCP_SERVER_H */
