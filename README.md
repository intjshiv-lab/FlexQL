# FlexQL

> A from-scratch SQL database engine with persistence. C++17, zero dependencies, ~7,400 lines.

I built FlexQL to understand what actually happens inside a database — from the byte-level storage layout all the way up to the TCP wire protocol. Everything is written by hand: the parser, the B+ tree, the arena allocator, the WAL, the snapshot system, the thread pool. No libraries, no shortcuts.

Benchmarked at **10 million rows** with numbers I'm pretty happy with.

---



### What's inside

```
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
```

- **Arena allocator** — bump-pointer allocation, avoids per-row `malloc` overhead
- **B+ tree** (order 256) — 3-level tree for 10M keys, doubly-linked leaves for range scans
- **Recursive-descent parser** — no yacc/bison, just a clean hand-rolled tokenizer + parser
- **LRU cache** — hash map + linked list, O(1) get/put, invalidated on writes
- **Thread pool + RW locks** — concurrent reads, exclusive writes
- **TTL** — per-row expiration, background sweep thread
- **TCP server** — length-prefixed wire protocol, multi-client
- **C API** — opaque handle (`flexql_open`, `flexql_exec`, `flexql_close`)

Supports `CREATE TABLE`, `INSERT`, `SELECT` (with `WHERE`), `INNER JOIN`. Four types: `INT`, `DECIMAL`, `VARCHAR`, `DATETIME`.

---

### Build & run

```bash
git clone https://github.com/intjshiv-lab/FlexQL.git
cd FlexQL/flexql
make all                   # builds server, client, tests, benchmark
```

```bash
./build/flexql_server &    # starts on port 9090 (in-memory mode)
./build/flexql_client      # interactive REPL
```

**With persistence (data survives restarts):**
```bash
./build/flexql_server --data-dir ./mydata &   # WAL + snapshots in ./mydata/
./build/flexql_client
# ... insert data, Ctrl+C the server, restart it — data is still there
```

```sql
FlexQL> CREATE TABLE users (id INT, name TEXT, age INT);
FlexQL> INSERT INTO users VALUES (1, 'Alice', 30);
FlexQL> SELECT * FROM users WHERE age > 25;
```

CMake also works if you prefer:
```bash
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

---

### Benchmarks

10M rows on benchmark system, `clang++ / gcc` with `-O3 -flto -march=native` and hot-path annotations:

| | Time | Throughput |
|---|---|---|
| INSERT 10M rows | 37.0 s | **~270K rows/sec** |
| SELECT * full scan | 7.59 s | **~1.32M rows/sec** |
| Point lookup (×100) | 23.45 ms | ~4.3K qps |
| Range scan (top 5%) | 181 ms | **~2.76M rows/sec** |
| INNER JOIN (100K × 1K) | 57.79 ms | **~1.73M rows/sec** |

```bash
make benchmark && ./build/flexql_bench
```

---

### Tests

92 test cases across 11 suites. Everything from arena alignment edge cases to full end-to-end query pipelines and persistence recovery.

```bash
make run-tests
```

| | |
|---|---|
| `test_arena` | alloc, alignment, reset |
| `test_schema` | column defs, type checks |
| `test_storage` | insert, retrieve, delete |
| `test_parser` | tokenization, AST output |
| `test_bptree` | insert, search, range, delete |
| `test_executor` | CREATE/INSERT/SELECT/JOIN |
| `test_cache` | LRU eviction, capacity |
| `test_integration` | end-to-end pipelines |
| `test_wal` | CRC32, write/read, replay, corruption |
| `test_snapshot` | save/load, multi-table, round-trip |
| `test_persistence` | full recovery, WAL replay, in-memory mode |

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
├── tests/
├── benchmark/
├── CMakeLists.txt
└── Makefile
```

---

### Design document

The full design rationale (1200+ lines) is in [`DesignDoc.md`](DesignDoc.md) — covers every major decision, rejected alternatives, ASCII diagrams of data structures, persistence architecture, and benchmark analysis.

### Known limitations

- No `DELETE`/`UPDATE` yet (only `CREATE`, `INSERT`, `SELECT`)
- Single `WHERE` condition (no `AND`/`OR`)
- No aggregates (`COUNT`, `SUM`, etc.) or `ORDER BY`
- Only primary key indexing (first column)

These are all "future work" — the core engine is solid and extensible.

### Persistence

FlexQL v2.0 adds disk persistence via **Write-Ahead Log (WAL)** and **binary snapshots**:

- **WAL**: Every mutating SQL (`CREATE TABLE`, `INSERT`) is appended with CRC32 integrity before execution
- **Snapshots**: Binary checkpoint of all tables on clean shutdown (or manual trigger)
- **Recovery**: Load last snapshot → replay WAL from that point → fully restored
- **Backward-compatible**: Omit `--data-dir` and it runs as a pure in-memory engine

### License

MIT — see [LICENSE](LICENSE).

---

Built by [Ramesh Choudhary](https://github.com/intjshiv-lab).
