/*
 * main.cpp — server entry point
 *
 * Usage: flexql_server [--data-dir /path] [--port N] [--threads N]
 *
 * --data-dir  : Enable persistent storage (WAL + snapshot) in this directory.
 *               If omitted, runs in pure in-memory mode (original behavior).
 * --port      : TCP port (default 9876)
 * --threads   : Worker threads (default 8)
 */

#include "server/database.h"
#include "server/network/tcp_server.h"
#include "common.h"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <string>

static flexql::TCPServer* g_server = nullptr;
static flexql::Database*  g_db     = nullptr;

static void signal_handler(int) {
    if (g_server) g_server->stop();
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --data-dir PATH  Enable persistence (WAL + snapshot) in PATH\n"
              << "  --port N         TCP port (default: 9876)\n"
              << "  --threads N      Worker threads (default: 8)\n"
              << "  --help           Show this message\n";
}

int main(int argc, char* argv[]) {
    uint16_t    port        = flexql::DEFAULT_PORT;
    size_t      num_threads = flexql::THREAD_POOL_SIZE;
    std::string data_dir;

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            num_threads = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            // Legacy positional args: [port] [threads]
            if (i == 1) port = static_cast<uint16_t>(std::atoi(argv[i]));
            if (i == 2) num_threads = static_cast<size_t>(std::atoi(argv[i]));
        }
    }

    std::cout << "========================================\n";
    std::cout << "  FlexQL Server v2.0\n";
    std::cout << "  Port:        " << port << "\n";
    std::cout << "  Threads:     " << num_threads << "\n";
    if (!data_dir.empty()) {
        std::cout << "  Data Dir:    " << data_dir << "\n";
        std::cout << "  Persistence: ENABLED (WAL + Snapshot)\n";
    } else {
        std::cout << "  Persistence: OFF (in-memory only)\n";
    }
    std::cout << "========================================\n";

    flexql::Database db(data_dir);
    g_db = &db;
    db.start();

    flexql::TCPServer server(db.executor(), port, num_threads);
    g_server = &server;

    // Handle Ctrl-C gracefully
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.run();  // Blocking

    // On shutdown: snapshot + stop
    if (db.persistent()) {
        std::string err;
        if (db.take_snapshot(err)) {
            std::cout << "[FlexQL] Final snapshot saved.\n";
        } else {
            std::cerr << "[FlexQL] Snapshot error: " << err << "\n";
        }
    }

    db.stop();
    std::cout << "\n[FlexQL] Server shut down.\n";
    return 0;
}
