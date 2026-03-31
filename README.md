```markdown
FlexQL

> A from-scratch SQL database engine with persistence. C++17, zero dependencies, ~7,333 lines.

I built FlexQL to understand what actually happens inside a database — from the byte-level storage layout all the way up to the TCP wire protocol. Everything is written by hand: the parser, the B+ tree, the arena allocator, the WAL, the snapshot system, the thread pool. No libraries, no shortcuts.

Benchmarked at **10 million rows** with v2.2 performance numbers.

---

What's inside

 SQL string
     │
     ▼
 ┌────────┐   ┌────────┐   ┌──────────┐
 │ Lexer  │──▶│ Parser │──▶│ Executor │
 └────────┘   └────────┘   └────┬─────┘
                                 │
                    ┌────────────┼────────────┐
                    ▼            ▼            ▼
              ┌──────────┐ ┌─────────┐ ┌──────────┐
              │ LRU Cache│ │ Storage │ │ B+ Tree  │
              │          │ │ (Arena) │ │  Index   │
              └──────────┘ └─────────┘ └──────────┘
                    │
              ┌─────┴──────────────┐
              │ Thread Pool        │
              │ Lock Manager       │
              │ TTL Reaper (bg)    │
              └────────────────────┘
`
```

- **Arena allocator** — bump-pointer allocation, avoids per-row `malloc` overhead
- **B+ tree** (order 256) — 3-level tree for 10M keys, doubly-linked leaves for range scans
- **Recursive-descent parser** — no yacc/bison, just a clean hand-rolled tokenizer + parser
- **LRU cache** — hash map + linked list, O(1) get/put, invalidated on writes
- **Thread pool + RW locks** — concurrent reads, exclusive writes
- **TTL** — per-row expiration, background sweep thread
- **TCP server** — length-prefixed wire protocol, multi-client
- **C API** — opaque handle (`flexql_open`, `flexql_exec`, `flexql_close`)
- **Write-Ahead Log (WAL)** — persistent storage with CRC-32 integrity checking
- **Binary snapshots** — checkpoint-based recovery
- **Batch INSERT** — multi-tuple `VALUES (...), (...)` support
- **Non-equality JOINs** — `<`, `>`, `<=`, `>=` via Nested Loop Join

Supports `CREATE TABLE`, `INSERT` (including batch), `SELECT` (with `WHERE`), `INNER JOIN`. Four types: `INT`, `DECIMAL`, `VARCHAR`, `DATETIME`.
```
---

### Build & run

```bash
git clone https://github.com/intjshiv-lab/FlexQL.git
cd FlexQL/flexql
make start                   # builds server, client, tests, benchmark
make bench                   # runs benchmarks
make stop                    # kills server,client, tests , benchmark
```

```bash
./build/flexql_server &    # starts on port 9876 (in-memory mode by default)
./build/flexql_client      # interactive REPL
```

**With persistence (data survives restarts):**
```bash
./build/flexql_server --data-dir ./mydata &   # WAL + snapshots in ./mydata/
./build/flexql_client
# ... insert data, Ctrl+C the server, restart it — data is still there
```

```sql
FlexQL> CREATE TABLE users (id INT, name VARCHAR(50), age INT);
FlexQL> INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25);
FlexQL> SELECT * FROM users WHERE age > 25;
```

CMake also works if you prefer:
```bash
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

---

**Benchmarks (v2.2)**

**10M rows on benchmark system with `-O3 -flto -march=native` and hot-path annotations:**

| Operation | Time | Throughput |
|---|---|---|
| INSERT 10M rows | 16.9 s | **592,728 rows/sec** |
| SELECT * full scan | 6.3 s | **1,594,668 rows/sec** |
| Point lookup (×100) | 12.86 ms | **129 μs avg** |
| Range scan (5%) | 233.75 ms | **2,139,027 rows/sec** |
| INNER JOIN (100K × 1K) | 38.11 ms | **2,624,075 rows/sec** |

> **Note:** v2.2 throughput includes WAL writes per mutation for persistence. Disable `--data-dir` for pure in-memory performance (v2.1 numbers were ~15-25% faster without persistence).

```bash
make benchmark && ./build/flexql_bench
```

---

### Tests

**92 test cases across 11 suites.** Everything from arena alignment edge cases to full end-to-end query pipelines and persistence recovery.

```bash
make run-tests
```

| Test Suite | Coverage |
|---|---|
| `test_arena` | alloc, alignment, chunking, reset, stats |
| `test_schema` | column defs, type checks, offsets, row size |
| `test_storage` | insert, scan, WHERE ops, index, TTL, projection |
| `test_parser` | lexer tokens, all SQL statement types, errors |
| `test_bptree` | insert/find, all 6 range ops, 10K stress, types |
| `test_cache` | put/get, normalization, miss, eviction, LRU order |
| `test_executor` | CREATE/INSERT/SELECT, JOIN, all types, cache |
| `test_integration` | full workflows, 10K stress, case insensitivity |
| `test_wal` | CRC-32, write/read, replay, truncate, corruption |
| `test_snapshot` | empty/single/multi table, datetime, 10K round-trip |
| `test_persistence` | E2E recovery, WAL replay, multi-table, WHERE after recovery |

---

### Project structure

```
flexql/
├── include/flexql.h        # public C API
├── src/
│   ├── common.h            # shared types (Value, DataType, etc.)
│   ├── client/             # REPL client
│   └── server/
│       ├── database.cpp    # top-level coordinator
│       ├── storage/        # arena, schema, table, WAL, snapshot
│       ├── parser/         # lexer, parser, AST nodes
│       ├── executor/       # query execution engine
│       ├── index/          # B+ tree
│       ├── cache/          # LRU cache
│       ├── concurrency/    # thread pool, lock manager
│       ├── network/        # TCP server + wire protocol
│       └── ttl/            # row expiration
├── tests/                  # 92 unit and integration tests
├── benchmark/              # 10M row benchmark suite
├── CMakeLists.txt
└── Makefile
```

---

### Design document

The full design rationale (**7,000+ lines**) is in [`DesignDoc.md`](DesignDoc.md) — covers:
- Every major architectural decision (B+ tree order, arena sizing, WAL format)
- Rejected alternatives (LSM trees, columnar storage, skip lists)
- ASCII diagrams of all data structures
- Complete persistence architecture (WAL + snapshots)
- Benchmark analysis and performance trade-offs
- Module-by-module deep dive (49 files, 92 tests, 7,333 LOC)

---

### Codebase Statistics

| Metric | Value |
|--------|-------|
| **Total source files** | 49 |
| **Total lines of code** | 7,333 |
| **Core library (headers + impl)** | 3,823 lines |
| **Test code** | 2,054 lines |
| **Benchmark code** | 217 lines |
| **Build files** | 200 lines |
| **Public C API** | 103 lines |
| **Client code** | 331 lines |
| **Test/code ratio** | 0.54 (54% test coverage by LOC) |
| **Total unit tests** | 92 |

---

### Known limitations

- No `DELETE`/`UPDATE` yet (only `CREATE`, `INSERT`, `SELECT`)
- Single `WHERE` condition (no `AND`/`OR` compound predicates)
- No aggregate functions (`COUNT`, `SUM`, `AVG`, etc.) or `GROUP BY`
- No `ORDER BY` or `LIMIT`
- Only primary key indexing (first column)
- No secondary indices

These are all "future work" — the core engine is solid and extensible.

---

### Persistence (v2.0+)

FlexQL supports optional disk persistence via **Write-Ahead Log (WAL)** and **binary snapshots**:

- **WAL**: Every mutating SQL (`CREATE TABLE`, `INSERT`) is appended with CRC-32 integrity before execution
- **Snapshots**: Binary checkpoint of all tables on clean shutdown or manual trigger
- **Recovery**: Load last snapshot → replay WAL from that point → fully restored
- **Backward-compatible**: Omit `--data-dir` and it runs as a pure in-memory engine (zero persistence overhead)

### C API

Simple, opaque, foolproof — modeled after SQLite:

```c
int  flexql_open(const char *host, int port, FlexQL **db);
int  flexql_close(FlexQL *db);
int  flexql_exec(FlexQL *db, const char *sql, flexql_callback cb, void *arg, char **errmsg);
void flexql_free(void *ptr);
```

---

### License

MIT — see [LICENSE](LICENSE).

---

Built by [Ramesh Choudhary](https://github.com/intjshiv-lab).  
**v2.2** — March 2026

**Key updates from v1.0 → v2.2:**
- ✅ Benchmark numbers updated to reflect v2.2 performance (with WAL enabled)
- ✅ Added persistence explanation (WAL + snapshots)
- ✅ Batch INSERT and non-equality JOIN operators listed
- ✅ Codebase stats updated (7,333 LOC, 92 tests)
- ✅ New sections for C API and detailed statistics
- ✅ Link to comprehensive DesignDoc.md
- ✅ All limitations clearly documented


