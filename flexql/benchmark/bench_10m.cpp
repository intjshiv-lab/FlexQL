/*
 * bench_10m.cpp — 10 million row benchmark
 *
 * Tests INSERT, full scan, point lookup, range scan, and JOIN.
 * Prints results as JSON so we can track regressions easily.
 */

#include "server/executor/executor.h"
#include "common.h"
#include <cassert>
#include <iostream>
#include <chrono>
#include <random>
#include <iomanip>
#include <string>
#include <fstream>

using namespace flexql;
using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
    std::string name;
    double      elapsed_ms;
    size_t      row_count;
    double      rows_per_sec;
};

static void print_result(const BenchResult& br) {
    std::cout << std::left << std::setw(35) << br.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2)
              << br.elapsed_ms << " ms"
              << std::setw(14) << br.row_count << " rows"
              << std::setw(16) << std::fixed << std::setprecision(0)
              << br.rows_per_sec << " rows/s"
              << "\n";
}

int main(int argc, char* argv[]) {
    size_t N = 10'000'000;  // Default: 10M
    if (argc >= 2) N = static_cast<size_t>(std::atol(argv[1]));

    std::cout << "============================================\n";
    std::cout << "  FlexQL Benchmark — " << N << " rows\n";
    std::cout << "============================================\n\n";

    Executor exec;
    std::vector<BenchResult> results;

    // -------------------------------------------------------
    // 1. CREATE TABLE
    // -------------------------------------------------------
    {
        auto t0 = Clock::now();
        auto r = exec.execute_sql("CREATE TABLE bench (id INT, val DECIMAL, name VARCHAR(20))");
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        assert(r.success);
        std::cout << "CREATE TABLE: " << ms << " ms\n\n";
    }

    // -------------------------------------------------------
    // 2. INSERT N rows
    // -------------------------------------------------------
    {
        std::cout << "Inserting " << N << " rows...\n";
        auto t0 = Clock::now();

        constexpr size_t BATCH_SIZE = 1000;
        std::string batch_sql;
        batch_sql.reserve(BATCH_SIZE * 50);

        for (size_t i = 0; i < N; ++i) {
            if (i % BATCH_SIZE == 0) {
                batch_sql = "INSERT INTO bench VALUES ";
            }

            batch_sql += "(" + std::to_string(i) + ", " + 
                         std::to_string(i * 0.001) + ", 'row" + 
                         std::to_string(i) + "')";

            if ((i + 1) % BATCH_SIZE == 0 || i == N - 1) {
                auto r = exec.execute_sql(batch_sql);
                if (!r.success) {
                    std::cerr << "INSERT failed at row " << i << ": " << r.error << "\n";
                    return 1;
                }
            } else {
                batch_sql += ", ";
            }

            if (i % 1'000'000 == 0 && i > 0) {
                auto tn = Clock::now();
                double elapsed = std::chrono::duration<double>(tn - t0).count();
                std::cout << "  " << i << " rows inserted (" << std::fixed
                          << std::setprecision(1) << elapsed << " s)\n";
            }
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        BenchResult br{"INSERT " + std::to_string(N) + " rows", ms, N, N / (ms / 1000.0)};
        results.push_back(br);
        print_result(br);
        std::cout << "\n";
    }

    // -------------------------------------------------------
    // 3. SELECT * (full scan)
    // -------------------------------------------------------
    {
        std::cout << "Full scan (SELECT *)...\n";
        auto t0 = Clock::now();
        auto r = exec.execute_sql("SELECT * FROM bench");
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        assert(r.success);
        BenchResult br{"SELECT * (full scan)", ms, r.rows.size(),
                       r.rows.size() / (ms / 1000.0)};
        results.push_back(br);
        print_result(br);
        std::cout << "\n";
    }

    // -------------------------------------------------------
    // 4. Point lookups (100 random)
    // -------------------------------------------------------
    {
        std::cout << "Point lookups (100 random)...\n";
        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> dist(0, N - 1);

        auto t0 = Clock::now();
        for (int i = 0; i < 100; ++i) {
            size_t id = dist(rng);
            auto r = exec.execute_sql("SELECT * FROM bench WHERE id = " + std::to_string(id));
            assert(r.success && r.rows.size() == 1);
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        BenchResult br{"Point lookup (100 queries)", ms, 100, 100 / (ms / 1000.0)};
        results.push_back(br);
        print_result(br);
        std::cout << "  Avg per query: " << (ms / 100.0) << " ms\n\n";
    }

    // -------------------------------------------------------
    // 5. Range scan (WHERE id > 95% of N)
    // -------------------------------------------------------
    {
        size_t threshold = static_cast<size_t>(N * 0.95);
        std::cout << "Range scan (id > " << threshold << ")...\n";

        auto t0 = Clock::now();
        auto r = exec.execute_sql("SELECT * FROM bench WHERE id > " + std::to_string(threshold));
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        assert(r.success);
        BenchResult br{"Range scan (top 5%)", ms, r.rows.size(),
                       r.rows.size() / (ms / 1000.0)};
        results.push_back(br);
        print_result(br);
        std::cout << "\n";
    }

    // -------------------------------------------------------
    // 6. JOIN benchmark (smaller tables for timing)
    // -------------------------------------------------------
    {
        std::cout << "JOIN benchmark...\n";
        exec.execute_sql("CREATE TABLE join_a (id INT, val INT)");
        exec.execute_sql("CREATE TABLE join_b (id INT, label VARCHAR(20))");

        size_t join_n = std::min(N, (size_t)100000);
        for (size_t i = 0; i < join_n; ++i) {
            exec.execute_sql("INSERT INTO join_a VALUES (" + std::to_string(i) + ", " +
                             std::to_string(i % 1000) + ")");
        }
        for (size_t i = 0; i < 1000; ++i) {
            exec.execute_sql("INSERT INTO join_b VALUES (" + std::to_string(i) + ", 'label" +
                             std::to_string(i) + "')");
        }

        auto t0 = Clock::now();
        auto r = exec.execute_sql(
            "SELECT * FROM join_a INNER JOIN join_b ON join_a.val = join_b.id");
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        assert(r.success);
        BenchResult br{"INNER JOIN (" + std::to_string(join_n) + " x 1K)", ms,
                       r.rows.size(), r.rows.size() / (ms / 1000.0)};
        results.push_back(br);
        print_result(br);
        std::cout << "\n";
    }

    // -------------------------------------------------------
    // Summary
    // -------------------------------------------------------
    std::cout << "\n============================================\n";
    std::cout << "  BENCHMARK SUMMARY\n";
    std::cout << "============================================\n";
    for (const auto& br : results) {
        print_result(br);
    }

    // Write JSON report
    std::ofstream json("benchmark_results.json");
    if (json.is_open()) {
        json << "{\n  \"rows\": " << N << ",\n  \"results\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            json << "    {\"name\": \"" << results[i].name << "\", "
                 << "\"elapsed_ms\": " << std::fixed << std::setprecision(2)
                 << results[i].elapsed_ms << ", "
                 << "\"row_count\": " << results[i].row_count << ", "
                 << "\"rows_per_sec\": " << std::setprecision(0)
                 << results[i].rows_per_sec << "}";
            if (i + 1 < results.size()) json << ",";
            json << "\n";
        }
        json << "  ]\n}\n";
        json.close();
        std::cout << "\nResults written to benchmark_results.json\n";
    }

    return 0;
}
