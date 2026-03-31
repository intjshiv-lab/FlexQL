/*
 * main.cpp — client REPL
 *
 * Interactive shell that connects to the server and lets you
 * type SQL. Basically a glorified readline loop.
 *
 * Usage: flexql_client [host] [port]
 */

#include "flexql.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

// Callback to print SELECT results
static int print_callback(void* /*user_data*/, int ncols, char** values, char** col_names) {
    // Print header on first row (we use a static flag, reset per query)
    static bool header_printed = false;
    if (!header_printed) {
        for (int i = 0; i < ncols; ++i) {
            if (i > 0) std::cout << "\t";
            std::cout << (col_names[i] ? col_names[i] : "?");
        }
        std::cout << "\n";
        // Separator
        for (int i = 0; i < ncols; ++i) {
            if (i > 0) std::cout << "\t";
            std::cout << "--------";
        }
        std::cout << "\n";
        header_printed = true;
    }

    for (int i = 0; i < ncols; ++i) {
        if (i > 0) std::cout << "\t";
        std::cout << (values[i] ? values[i] : "NULL");
    }
    std::cout << "\n";
    return 0;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 9876;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = std::atoi(argv[2]);

    std::cout << "FlexQL Client v1.0\n";
    std::cout << "Connecting to " << host << ":" << port << "...\n";

    FlexQL* db = nullptr;
    int rc = flexql_open(host.c_str(), port, &db);
    if (rc != FLEXQL_OK) {
        std::cerr << "Failed to connect to server at " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "Connected. Type SQL queries or EXIT to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << "flexql> ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;

        // Trim
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
               line.back() == '\n' || line.back() == '\r' || line.back() == ';'))
            line.pop_back();

        if (line.empty()) continue;

        // Check for local commands
        {
            std::string upper = line;
            for (auto& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (upper == "EXIT" || upper == "QUIT" || upper == "\\Q") break;
        }

        char* errmsg = nullptr;
        rc = flexql_exec(db, line.c_str(), print_callback, nullptr, &errmsg);
        if (rc != FLEXQL_OK) {
            if (errmsg) {
                std::cerr << "Error: " << errmsg << "\n";
                flexql_free(errmsg);
            } else {
                std::cerr << "Error executing query.\n";
            }
        }
        std::cout << "\n";
    }

    flexql_close(db);
    std::cout << "Goodbye.\n";
    return 0;
}
