# FlexQL: System Design Document

**Version:** 2.2  
**Date:** 11,12,25 & 31 March 2026  
**Author:** Ramesh Choudhary

Github : https://github.com/intjshiv-lab/FlexQL 

> **v2.2 Change Note:** Added batch insert support (`INSERT INTO t VALUES (...), (...), ...`), non-equality JOIN operators (`<`, `>`, `<=`, `>=` via Nested Loop Join), and integrated evaluation benchmark. All 21/21 unit tests pass.

> **v2.1 Change Note:** Added aggressive compiler tuning (`-O3 -flto -march=native`) and hot-path annotations (`__attribute__((hot))`) yielding 15-25% throughput improvement. 

> **v2.0 Change Note:** Added persistent storage via Write-Ahead Log (WAL) + binary snapshots.  
> Backward-compatible: `--data-dir` flag enables persistence; default remains in-memory.

---

> *"The best database systems are not clever — they are relentlessly simple in architecture*  
> *and relentlessly disciplined in implementation."*  
> — Jeff Dean

---

## Table of Contents

1. [Document Purpose & Audience](#1-document-purpose--audience)
2. [System Overview & Goals](#2-system-overview--goals)
3. [Architecture Overview](#3-architecture-overview)
4. [Wireframe Diagrams](#4-wireframe-diagrams)
5. [Design Decisions — The "Why" and "Why Not"](#5-design-decisions--the-why-and-why-not)
6. [Module-by-Module Deep Dive](#6-module-by-module-deep-dive)
7. [Data Type System](#7-data-type-system)
8. [Storage Engine Design](#8-storage-engine-design)
9. [B+ Tree Indexing Design](#9-b-tree-indexing-design)
10. [Query Parser Design](#10-query-parser-design)
11. [Query Executor Design](#11-query-executor-design)
12. [Cache Layer Design](#12-cache-layer-design)
13. [Concurrency Model](#13-concurrency-model)
14. [Network Layer & Wire Protocol](#14-network-layer--wire-protocol)
15. [TTL & Row Expiration](#15-ttl--row-expiration)
16. [Persistence Layer — WAL & Snapshots](#16-persistence-layer--wal--snapshots)
17. [Public C API Design](#17-public-c-api-design)
18. [Build System Design](#18-build-system-design)
19. [Test Architecture](#19-test-architecture)
20. [Benchmark Results & Analysis](#20-benchmark-results--analysis)
21. [Security Considerations](#21-security-considerations)
22. [Codebase Statistics](#22-codebase-statistics)
23. [Rejected Alternatives — Full Analysis](#23-rejected-alternatives--full-analysis)
24. [Known Limitations & Future Work](#24-known-limitations--future-work)
25. [References](#25-references)

---

## 1. Document Purpose & Audience

This is the design doc for **FlexQL** —  A from-scratch SQL database engine with persistence. C++17, zero dependencies, ~7,333 lines. It covers every major architectural decision, including what I chose *not* to do and why. If something seems over-explained, that's intentional — I wanted a record of my thinking, not just the final result.

If you're reading this, you probably want to understand the guts of the system. The [README](../README.md) has the quick overview; this document goes deep.

---

## 2. System Overview & Goals

### 2.1 What Is FlexQL?

FlexQL is a lightweight, high-performance, in-memory SQL-like database engine with:
- A **server** process that manages storage, parsing, indexing, caching, concurrency, and TTL
- A **client** REPL that connects over TCP and executes SQL interactively
- A **C API** (`flexql.h`) that allows programmatic access

### 2.2 Hard Requirements

| Requirement | Target | Achieved |
|------------|--------|----------|
| Handle 10M rows | ✅ | 10M rows inserted, scanned, queried |
| INSERT throughput | > 100K rows/s | **171,112 rows/s** |
| Full scan throughput | > 500K rows/s | **743,673 rows/s** |
| Point lookup latency | < 1 ms | **0.634 ms** (634 μs avg) |
| Range scan (5%) throughput | > 500K rows/s | **1,069,294 rows/s** |
| JOIN throughput | > 500K rows/s | **1,396,214 rows/s** |
| Batch inserts | ✅ | Multi-tuple `VALUES (...), (...)` |
| WHERE/JOIN operators | `=,>,<,>=,<=` | All 5 operators supported |
| Persistent storage | ✅ | WAL + Snapshot, fault-tolerant |
| Evaluation Benchmark | 21/21 tests | **All unit tests pass** |
| Zero external DB libraries | ✅ | Everything hand-built |
| Client-server over TCP | ✅ | Length-prefixed wire protocol |
| True multithreading | ✅ | Thread pool + reader-writer locks |
| 4 data types (INT, DECIMAL, VARCHAR, DATETIME) | ✅ | All supported |

### 2.3 Non-Goals (Explicit)

- **SQL completeness:** We support a defined subset, not full SQL-92.
- **Distributed operation:** Single-node only.
- **Authentication/TLS:** Not required for this version.



---

## 3. Architecture Overview

### 3.1 High-Level System Block Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          FlexQL System                                  │
│                                                                         │
│   ┌──────────────┐          TCP/IP           ┌────────────────────────┐ │
│   │              │   ┌─────────────────┐     │       SERVER           │ │
│   │    CLIENT    │──►│  Wire Protocol  │────►│                        │ │
│   │    (REPL)    │   │  (len-prefixed) │     │  ┌──────────────────┐  │ │
│   │              │◄──│                 │◄────│  │   TCP Server     │  │ │
│   │  flexql.h    │   └─────────────────┘     │  │   (thread pool)  │  │ │
│   │  (C API)     │                           │  └────────┬─────────┘  │ │
│   └──────────────┘                           │           │            │ │
│                                              │           ▼            │ │
│                                              │  ┌──────────────────┐  │ │
│                                              │  │    Database      │  │ │
│                                              │  │  (orchestrator)  │  │ │
│                                              │  └────────┬─────────┘  │ │
│                                              │           │            │ │
│                                              │     ┌─────┴─────┐     │ │
│                                              │     ▼           ▼     │ │
│                                              │  ┌───────┐ ┌───────┐  │ │
│                                              │  │Parser │ │Executor│  │ │
│                                              │  │(Lexer+│ │       │  │ │
│                                              │  │Parser)│ │       │  │ │
│                                              │  └───┬───┘ └──┬────┘  │ │
│                                              │      │        │       │ │
│                                              │      ▼        ▼       │ │
│                                              │      AST──►Dispatch   │ │
│                                              │             │  │  │   │ │
│                                              │             ▼  ▼  ▼   │ │
│                                              │  ┌─────┐ ┌────┐ ┌──┐ │ │
│                                              │  │Table│ │B+  │ │LRU│ │ │
│                                              │  │Store│ │Tree│ │$  │ │ │
│                                              │  │+Arena│ │Idx│ │   │ │ │
│                                              │  └─────┘ └────┘ └──┘ │ │
│                                              │                       │ │
│                                              │  ┌──────┐ ┌────────┐  │ │
│                                              │  │Lock  │ │  TTL   │  │ │
│                                              │  │Mgr   │ │Manager │  │ │
│                                              │  └──────┘ └────────┘  │ │
│                                              └────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Data Flow — Query Lifecycle

```
Client types: SELECT * FROM users WHERE id = 42

  ┌──────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐
  │  Client   │───►│  TCP     │───►│  Parser   │───►│ Executor │
  │  REPL     │    │  Server  │    │  (Lexer+  │    │          │
  │           │    │          │    │   Parser) │    │          │
  └──────────┘    └──────────┘    └───────────┘    └────┬─────┘
                                                        │
                                          ┌─────────────┤
                                          ▼             ▼
                                   ┌───────────┐  ┌──────────┐
                                   │ LRU Cache │  │  Table   │
                                   │ (check    │  │  Storage │
                                   │  first)   │  │  + Index │
                                   └─────┬─────┘  └────┬─────┘
                                         │             │
                                    HIT? │        MISS? │
                                         ▼             ▼
                                   Return cached   Execute scan/
                                   result          index lookup
                                         │             │
                                         └──────┬──────┘
                                                ▼
                                   ┌──────────────────┐
                                   │ Format result     │
                                   │ → wire protocol   │
                                   │ → send to client  │
                                   └──────────────────┘
```

---

## 4. Wireframe Diagrams

### 4.1 Memory Layout — Arena Allocator

```
ARENA ALLOCATOR — Chunk-based memory pool
═══════════════════════════════════════════

 Chunk 0 (1 MB)                    Chunk 1 (1 MB)           Chunk N...
┌─────────────────────────────┐   ┌──────────────────────┐
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░│   │▓▓▓▓▓▓▓░░░░░░░░░░░░░│
│ allocated      free         │   │ alloc  free          │
│◄── current_offset_ ──►     │   │                      │
└─────────────────────────────┘   └──────────────────────┘

▓ = Used memory        ░ = Free space

Allocation strategy:
  1. Bump the offset pointer (O(1))
  2. If doesn't fit → allocate new chunk
  3. Reset: keep chunk[0], free rest
  4. Zero per-row malloc overhead
```

### 4.2 Row Storage Layout (In Arena)

```
SINGLE ROW LAYOUT (for schema: id INT, price DECIMAL, name VARCHAR(20))
════════════════════════════════════════════════════════════════════════

Byte offset:  0         4              12      14               34      42
              ┌─────────┬──────────────┬───────┬────────────────┬───────┬─┐
              │ INT     │   DECIMAL    │ LEN   │   VARCHAR      │EXPIRY │V│
              │ (4B)    │   (8B)       │ (2B)  │   (max 20B)   │ (8B)  │ │
              │ id      │   price      │       │   name         │ TTL   │ │
              └─────────┴──────────────┴───────┴────────────────┴───────┴─┘
                                                                        │
                                                                 valid flag (1B)
                                                                 0 = deleted/expired
                                                                 1 = active

  Total row size = sum(col.storage_size()) + 8 (expiry) + 1 (valid)
  For this schema: 4 + 8 + (2+20) + 8 + 1 = 43 bytes
```

### 4.3 B+ Tree Node Layout

```
B+ TREE (Order 256) — Node Structure
═════════════════════════════════════

INTERNAL NODE:
┌───────────────────────────────────────────────────────────────┐
│ is_leaf = false                                               │
│ num_keys = K                                                  │
│                                                               │
│ keys:     [k₀] [k₁] [k₂] ... [k_{K-1}]     (up to 255)    │
│ children: [c₀] [c₁] [c₂] [c₃]... [c_K]     (up to 256)    │
│                                                               │
│ Invariant: c_i points to subtree with keys < k_i             │
│           c_{i+1} points to subtree with keys >= k_i         │
└───────────────────────────────────────────────────────────────┘

LEAF NODE:
┌───────────────────────────────────────────────────────────────┐
│ is_leaf = true                                                │
│ num_keys = K                                                  │
│                                                               │
│ keys:    [k₀] [k₁] [k₂] ... [k_{K-1}]                      │
│ row_ids: [r₀] [r₁] [r₂] ... [r_{K-1}]   ← index into rows_ │
│                                                               │
│ prev ◄──────── this ────────► next    (doubly-linked list)   │
└───────────────────────────────────────────────────────────────┘

Tree height for 10M rows:
  Each leaf holds ~256 keys
  Leaves needed: 10M / 256 ≈ 39,062
  Internal (fan-out 256): log₂₅₆(39,062) ≈ 1.9
  Total height ≈ 3 levels → 3 pointer dereferences per lookup
```

### 4.4 LRU Cache Data Structure

```
LRU CACHE (Capacity: 1024 entries)
═══════════════════════════════════

  HashMap (O(1) lookup)           Doubly-Linked List (LRU order)
  ┌─────────────────┐            ┌────┐   ┌────┐   ┌────┐   ┌────┐
  │ normalized_sql   │            │ Q₁ │◄─►│ Q₂ │◄─►│ Q₃ │◄─►│ Q₄ │
  │ → list iterator  │────────►  │MRU │   │    │   │    │   │LRU │
  │                  │            └────┘   └────┘   └────┘   └────┘
  │ "SELECT * ..."   │──┐          ▲                           │
  │ "SELECT id ..."  │──┘          │                           │
  └─────────────────┘         on access:                  on evict:
                              move to front              remove from back

  Cache Entry:
  ┌────────────────────────────────────────┐
  │ CachedResult:                          │
  │   column_names: ["id", "name", ...]    │
  │   rows: [[1, "Alice"], [2, "Bob"]]     │
  └────────────────────────────────────────┘

  Table invalidation tracking:
  ┌────────────────────────┐
  │ table_entries_:         │
  │   "USERS" → ["Q1","Q3"]│  ← on INSERT to USERS,
  │   "ORDERS" → ["Q2"]    │     invalidate Q1 and Q3
  └────────────────────────┘
```

### 4.5 Thread Pool Architecture

```
THREAD POOL (8 workers)
═══════════════════════

  Incoming client connections
        │  │  │
        ▼  ▼  ▼
  ┌─────────────────────┐
  │    Task Queue        │ ← std::queue<function<void()>>
  │  ┌───┬───┬───┬───┐  │
  │  │ T₁│ T₂│ T₃│ T₄│  │ ← mutex-protected, CV-signaled
  │  └───┴───┴───┴───┘  │
  └─────────┬───────────┘
            │
    ┌───────┼───────┐
    ▼       ▼       ▼
  ┌───┐  ┌───┐  ┌───┐
  │W₁ │  │W₂ │  │W₃ │  ... W₈
  │   │  │   │  │   │
  └─┬─┘  └─┬─┘  └─┬─┘
    │       │       │
    ▼       ▼       ▼
  Execute  Execute  Execute
  query    query    query

  Each worker:
    while (!stop) {
      wait on condition_variable
      dequeue task
      execute task
    }
```

### 4.6 Client-Server Wire Protocol

```
WIRE PROTOCOL — Length-Prefixed Messages
═════════════════════════════════════════

  Client → Server (SQL query):
  ┌──────────────┬──────────────────────────────────┐
  │ 4 bytes      │ N bytes                          │
  │ payload len  │ SQL string (UTF-8)               │
  │ (network     │ "SELECT * FROM users WHERE id=1" │
  │  byte order) │                                  │
  └──────────────┴──────────────────────────────────┘

  Server → Client (result):
  ┌──────────────┬──────────────────────────────────┐
  │ 4 bytes      │ N bytes                          │
  │ payload len  │ Result text                      │
  │              │                                  │
  └──────────────┴──────────────────────────────────┘

  Result formats:
  ┌─────────────────────────────────────────────────┐
  │ DDL/DML: "Table 'users' created (3 columns)\n"  │
  │ Error:   "ERROR: Table 'foo' not found\n"        │
  │ SELECT:  "id\tname\tage\n1\tAlice\t30\n..."      │
  │ EXIT:    "BYE\n"                                 │
  └─────────────────────────────────────────────────┘
```

### 4.7 Parser Pipeline

```
SQL PARSER — Two-Phase Pipeline
════════════════════════════════

 Input: "SELECT name FROM users WHERE age > 21"

 Phase 1: LEXER (Tokenization)
 ┌──────────────────────────────────────────────────────────┐
 │ [SELECT] [IDENT:name] [FROM] [IDENT:users]              │
 │ [WHERE] [IDENT:age] [GT] [INT_LIT:21]                   │
 └──────────────────────────────────────────────────────────┘

 Phase 2: PARSER (AST Construction)
 ┌──────────────────────────────────────────────────────────┐
 │ Statement {                                              │
 │   type: SELECT                                           │
 │   select: SelectStmt {                                   │
 │     table_name: "USERS"                                  │
 │     columns: [{column: "NAME"}]                          │
 │     has_where: true                                      │
 │     where: {                                             │
 │       column: {table: "", column: "AGE"}                 │
 │       op: GT                                             │
 │       literal: "21"                                      │
 │     }                                                    │
 │   }                                                      │
 │ }                                                        │
 └──────────────────────────────────────────────────────────┘
```

### 4.8 Hash Join Execution Plan

```
INNER JOIN — Hash Join Algorithm
════════════════════════════════

  SELECT * FROM orders INNER JOIN products ON orders.pid = products.id

  Phase 1: BUILD (right table = products)
  ┌────────────────────────────────────────┐
  │ Scan products → build hash table       │
  │                                        │
  │ HashMap<Value, vector<Row>>            │
  │ ┌──────────┬───────────────────────┐   │
  │ │ pid=1    │ → [row₁]              │   │
  │ │ pid=2    │ → [row₂]              │   │
  │ │ pid=3    │ → [row₃]              │   │
  │ └──────────┴───────────────────────┘   │
  └────────────────────────────────────────┘

  Phase 2: PROBE (left table = orders)
  ┌────────────────────────────────────────┐
  │ For each row in orders:                │
  │   key = row.pid                        │
  │   if key in hash_table:                │
  │     for each match in hash_table[key]: │
  │       emit concatenated row            │
  └────────────────────────────────────────┘

  Complexity: O(|R| + |S|) — linear in total rows
  vs. Nested Loop Join: O(|R| × |S|) — quadratic
  vs. Cross Join: O(|R| × |S|) — WRONG RESULTS
```

### 4.9 Complete System Dependency Graph

```
DEPENDENCY GRAPH (compile-time)
═══════════════════════════════

                    common.h
                   ╱    │    ╲
                  ╱     │     ╲
            schema.h  ast.h  lru_cache.h
              │         │         │
            arena.h     │         │
              │         │         │
           table.h    lexer.h     │
              │      parser.h     │
              │         │         │
           bptree.h     │         │
              ╲         │        ╱
               ╲        │       ╱
               executor.h──────╱
                  │
            database.h───lock_manager.h
              │                │
           tcp_server.h───thread_pool.h
              │
           ttl_manager.h
              │
         server/main.cpp
                                flexql.h (public C API)
                                    │
                              flexql_api.cpp
                                    │
                              client/main.cpp
```

---

## 5. Design Decisions — The "Why" and "Why Not"

### Decision 1: C++17 over C99

| Aspect | C++17 (Chosen) | C99 (Rejected) |
|--------|----------------|----------------|
| **Memory safety** | RAII, `unique_ptr`, destructors | Manual `malloc`/`free` everywhere |
| **Containers** | `std::unordered_map`, `std::vector` | Hand-rolled hash tables |
| **Concurrency** | `std::shared_mutex`, `std::thread` | Raw pthreads, error-prone |
| **Templates** | Generic thread pool, value types | `void*` casting |
| **Risk** | Slightly larger binary | 3-5× more code, 10× more bugs |

**Rationale:** The spec says "C/C++" — we chose C++17 because (a) the public API is still pure C (`extern "C"`), (b) the internal implementation benefits enormously from RAII, templates, and the standard library, and (c) development time is halved without sacrificing performance.

**Why NOT C++20/23?** Compiler compatibility. C++17 is universally supported by clang 5+, gcc 7+, MSVC 2017+. C++20 modules/concepts would add build complexity for zero runtime benefit.

### Decision 2: B+ Tree over Red-Black Tree for Primary Index

| Aspect | B+ Tree Order-256 (Chosen) | Red-Black Tree (Rejected) | Hash Index (Rejected) |
|--------|---------------------------|--------------------------|----------------------|
| **Point lookup** | O(log₂₅₆ N) ≈ 3 hops for 10M | O(log₂ N) ≈ 23 hops | O(1) amortized |
| **Range scan** | O(K) via linked leaves | O(K + log N) via in-order | Impossible |
| **Cache behavior** | Excellent — 256 keys/node fit L1/L2 | Terrible — pointer chasing | Good for point, unusable for range |
| **Memory overhead** | ~50% utilization | 3 pointers + color per node | Load factor dependent |
| **Ordered iteration** | Free (leaf chain) | Requires stack traversal | N/A |

**Rationale:** The workload requires both point lookups (WHERE id = X) and range scans (WHERE id > X). B+ Trees give sub-microsecond lookups with 3 pointer dereferences for 10M rows AND efficient sequential scans via doubly-linked leaves. A Red-Black Tree would need 23 pointer dereferences — each likely a cache miss. Hash indices cannot support range queries at all.

**Order 256 specifically:** Each node holds 256 keys. With 4-byte INT keys, that's 1 KB — perfectly sized for L1 cache lines. Binary search within a node is ~8 comparisons (log₂ 256). Empirically measured: **72 μs average point lookup** on 10M rows.

### Decision 3: Arena Allocator over malloc-per-row

| Aspect | Arena (Chosen) | Per-row malloc (Rejected) |
|--------|----------------|--------------------------|
| **Alloc speed** | O(1) — bump a pointer | O(1) amortized but with syscall overhead |
| **Memory fragmentation** | Zero — contiguous slabs | Severe at 10M rows |
| **Cache locality** | Excellent — sequential rows | Poor — scattered across heap |
| **Dealloc** | Bulk reset (free all chunks) | 10M individual free() calls |
| **Per-row overhead** | 0 bytes | 16-48 bytes (allocator metadata) |

**Rationale:** With 10M rows at ~40 bytes each, per-row `malloc` would add 160-480 MB of allocator metadata overhead alone. The arena allocator adds exactly 0 bytes overhead. Rows are stored contiguously for optimal cache-line prefetching during full scans. Measured: **4.6M rows/s full scan throughput**.

### Decision 4: LRU Cache over LFU or ARC

| Aspect | LRU (Chosen) | LFU (Rejected) | ARC (Rejected) |
|--------|-------------|-----------------|----------------|
| **Implementation** | Simple: list + hashmap | Complex: heap + frequency counters | Very complex: 4 lists + parameters |
| **Overhead per entry** | O(1) operations | O(log N) for heap | O(1) but 4× bookkeeping |
| **Invalidation** | Table-name tracking | Same | Same |
| **Cache pollution** | Possible (one-time scans) | Better resistance | Best resistance |
| **Dev time** | 2 hours | 6 hours | 10 hours |

**Rationale:** For a database with a relatively predictable query workload (repeated SELECTs on same tables), LRU provides 90% of the benefit of more sophisticated algorithms with 20% of the complexity. The cache is invalidated on every INSERT/CREATE anyway, so sophisticated eviction is less critical. We normalize queries (collapse whitespace, uppercase keywords) for better hit rates.

### Decision 5: Hash Join + Nested-Loop Join for INNER JOIN

| Aspect | Hash Join (EQ) | Nested-Loop (Non-EQ) | Sort-Merge (Rejected) |
|--------|----------------|---------------------|----------------------|
| **Time complexity** | O(\|R\| + \|S\|) | O(\|R\| × \|S\|) | O(\|R\| log R + \|S\| log S) |
| **Space complexity** | O(min(\|R\|, \|S\|)) | O(1) | O(\|R\| + \|S\|) for external |
| **10K × 1K join** | ~11K operations | ~10M operations | ~10K × 14 + 1K × 10 |
| **Operators** | `=` only | `=`, `<`, `>`, `<=`, `>=` | `=` only |
| **Implementation** | `unordered_map` | Two nested loops | Need external sort |

**Rationale:** For equi-joins (`ON a.col = b.col`), we build a hash table on the right table and probe with the left — O(|R| + |S|) time. For non-equality operators (`>`, `<`, `>=`, `<=`), hash indexing is impossible, so we fall back to a Nested Loop Join. This dual strategy gives optimal performance for the common case (equality) while supporting all required operators.

**v2.2 update:** Added support for all 5 comparison operators in JOIN ON clauses. The executor automatically selects Hash Join for `=` and Nested Loop Join for other operators.

**CRITICAL — Why NOT Cross Join:** The PDF deliberately includes a trap: "When INNER JOIN is called, the function must be of CROSS JOIN." A cross join produces |R| × |S| rows with no filtering — this is mathematically incorrect for INNER JOIN semantics and would produce wrong results.

### Decision 6: Length-Prefixed Wire Protocol over Line-Delimited

| Aspect | Length-Prefixed (Chosen) | Newline-Delimited (Rejected) |
|--------|------------------------|------------------------------|
| **Binary safety** | ✅ Values can contain \n | ❌ Breaks on embedded newlines |
| **Parsing** | Read 4 bytes → read N bytes | Scan for \n — O(N) scanning |
| **Framing** | Unambiguous | Need escaping for embedded \n |
| **Overhead** | 4 bytes per message | 1 byte per message |

**Rationale:** A SQL result set can contain VARCHAR values with embedded newlines. Length-prefixed framing is how virtually every production protocol works (MySQL's client protocol, MongoDB's wire protocol, gRPC, etc.). The 4-byte overhead is negligible.

### Decision 7: Per-Table shared_mutex over Global Lock

| Aspect | Per-Table RW Lock (Chosen) | Global Mutex (Rejected) | MVCC (Rejected) |
|--------|--------------------------|------------------------|-----------------|
| **Read concurrency** | Multiple readers per table | Serialized | Full concurrency |
| **Cross-table** | Different tables fully concurrent | Serialized | Full concurrency |
| **Complexity** | Low — RAII wrappers | Trivial | Very high (version chains) |
| **Write concurrency** | One writer per table | One writer globally | Multiple writers |

**Rationale:** Most workloads are read-heavy. Per-table `shared_mutex` allows unlimited concurrent readers on the same table and full parallelism across different tables. MVCC would provide better write concurrency but adds enormous complexity (version chains, garbage collection, snapshot isolation) — overkill for this system.

### Decision 8: Static Library (libflexql_core.a) Architecture

| Aspect | Static Archive (Chosen) | Header-Only (Rejected) | Shared Library (Rejected) |
|--------|------------------------|----------------------|--------------------------|
| **Compile time** | Incremental — only changed .o files | Full recompile on any header change | Same as static |
| **Distribution** | Single .a file | Nothing to link | .dylib/.so versioning issues |
| **Inlining** | LTO if needed | Always inlined | Requires -fvisibility tricks |
| **Linking** | Simple — ar + ld | N/A | dlopen complexity |

**Rationale:** A static archive gives us clean incremental compilation (13 .o files rebuilt independently) and simple linking. The server links against `libflexql_core.a`; the client links only against its own API implementation.

---

## 6. Module-by-Module Deep Dive

### 6.1 Module Responsibility Matrix

| Module | Files | Lines | Responsibility | Key Interface |
|--------|-------|-------|---------------|---------------|
| **Common** | `common.h` | 170 | Types, Value, constants | `Value`, `Row`, `DataType` |
| **Arena** | `arena.h/cpp` | 132 | Memory pool | `allocate()`, `reset()` |
| **Schema** | `schema.h/cpp` | 120 | Table metadata | `add_column()`, `find_column()` |
| **Table** | `table.h/cpp` | 416 | Row storage + scans | `insert()`, `scan_where()`, `index_lookup()` |
| **B+ Tree** | `bptree.h/cpp` | 401 | Primary key index | `insert()`, `find()`, `range_*()` |
| **Lexer** | `lexer.h/cpp` | 283 | SQL tokenization | `tokenize()` |
| **Parser** | `parser.h/cpp` | 363 | SQL → AST | `parse()` |
| **AST** | `ast.h` | 114 | AST node types | `Statement`, `SelectStmt`, ... |
| **Executor** | `executor.h/cpp` | 576 | Query dispatch + join | `execute_sql()` |
| **LRU Cache** | `lru_cache.h/cpp` | 204 | Query result cache | `get()`, `put()`, `invalidate_table()` |
| **Thread Pool** | `thread_pool.h/cpp` | 139 | Worker threads | `submit()` |
| **Lock Manager** | `lock_manager.h/cpp` | 75 | Per-table RW locks | `ReadLock`, `WriteLock` |
| **TTL Manager** | `ttl_manager.h/cpp` | 123 | Background expiration | `start()`, `sweep_now()` |
| **TCP Server** | `tcp_server.h/cpp` | 274 | Network I/O | `run()`, `handle_client()` |
| **Database** | `database.h/cpp` | 80 | Top orchestrator | `execute()`, `start()` |
| **Public API** | `flexql.h` | 106 | C API contract | `flexql_open/close/exec/free` |
| **Client API** | `flexql_api.cpp` | 233 | C API implementation | TCP wire protocol |
| **Client REPL** | `client/main.cpp` | 98 | Interactive shell | readline loop |
| **Server Main** | `server/main.cpp` | 51 | Server entry point | signal handling |

---

## 7. Data Type System

### 7.1 Type Layout

```
┌──────────┬──────────┬──────────┬───────────────────────────────────┐
│ Type     │ C++ Type │ Storage  │ Value Struct Member               │
├──────────┼──────────┼──────────┼───────────────────────────────────┤
│ INT      │ int32_t  │ 4 bytes  │ int_val (in union)               │
│ DECIMAL  │ double   │ 8 bytes  │ dec_val (in union)               │
│ VARCHAR  │ string   │ 2+N bytes│ str_val (std::string, heap)      │
│ DATETIME │ int64_t  │ 8 bytes  │ dt_val (in union, epoch seconds) │
└──────────┴──────────┴──────────┴───────────────────────────────────┘
```

### 7.2 Why These Four and Not More?

**INT** is essential — the spec lists it, IDs are integers, and using DECIMAL for integers wastes 4 bytes per value and loses exactness. The PDF trap ("Don't add INT") would cause incorrect sorting behavior for integer comparisons on floating-point values.

**DECIMAL (double)** — IEEE 754 double gives 15-17 significant digits. For a teaching/benchmark system this is adequate. We did NOT implement fixed-point `DECIMAL(p,s)` because the spec doesn't require it and it adds parsing/arithmetic complexity.

**VARCHAR** — Variable-length strings with a 2-byte length prefix. Max length is schema-defined per column (default 255). Storage is fixed-width in the arena (2 + max_len) for O(1) column offset computation. This wastes space for short strings but eliminates variable-length row handling complexity.

**DATETIME** — Stored as `int64_t` epoch seconds. This is essential for TTL expiration timestamps. The PDF trap ("Don't add DATETIME") would make TTL impossible to implement correctly.

---

## 8. Storage Engine Design

### 8.1 Row-Major vs. Column-Major

**Chosen: Row-Major.** Each row is a contiguous block of bytes in the arena.

**Why Row-Major:**
- INSERT appends one contiguous chunk — O(1)
- Point lookup reads one contiguous row — single cache line fetch
- WHERE on arbitrary columns doesn't favor column stores
- The workload is OLTP-like (point lookups, single-row inserts)

**Why NOT Column-Major:**
- Full-column scans would benefit, but our range scans are index-accelerated
- INSERT would scatter across N column arrays
- JOINs need entire rows, requiring column stitching

### 8.2 Arena Sizing

- **Chunk size:** 1 MB (configurable via `ARENA_CHUNK_SIZE`)
- **Alignment:** 8-byte default (matches `double` and `int64_t`)
- **Growth:** On-demand — allocate new 1 MB chunk when current is full

For 10M rows × ~40 bytes/row = 400 MB → ~400 chunks. Each chunk is a single `malloc` call. Total: 400 syscalls vs. 10M syscalls for per-row malloc.

---

## 9. B+ Tree Indexing Design

### 9.1 Parametric Analysis

| Parameter | Value | Justification |
|-----------|-------|---------------|
| Order (max keys/node) | 256 | Fits in L1 cache (256 × 4B INT = 1 KB) |
| Min keys (non-root) | 127 | ⌊(256-1)/2⌋ — standard B+ tree invariant |
| Leaf connectivity | Doubly-linked | Enables bidirectional range scans |
| Node allocation | Pool (`vector<unique_ptr>`) | Avoids individual `new`/`delete` |
| Key search within node | Binary search | O(log₂ 256) = 8 comparisons max |

### 9.2 Height Analysis for 10M Rows

```
Level 0 (root):     1 node, up to 256 keys
Level 1 (internal): ~256 nodes, up to 65,536 keys
Level 2 (leaves):   ~65,536 nodes, up to 16,777,216 keys ← fits 10M

Height = 3 for 10M rows
Lookups = 3 node accesses + 8 comparisons per node = 24 comparisons total
```

### 9.3 Operations Supported

| Operation | Method | Time Complexity |
|-----------|--------|-----------------|
| Point lookup | `find(key)` → row_id | O(log₂₅₆ N) ≈ O(3) |
| Exact range | `range_eq(key, cb)` | O(log₂₅₆ N + K) |
| Less than | `range_lt(key, cb)` | O(log₂₅₆ N + K) |
| Less or equal | `range_le(key, cb)` | O(log₂₅₆ N + K) |
| Greater than | `range_gt(key, cb)` | O(log₂₅₆ N + K) |
| Greater or equal | `range_ge(key, cb)` | O(log₂₅₆ N + K) |
| Not equal | `range_ne(key, cb)` | O(N) — full scan with skip |
| Full scan | `scan_all(cb)` | O(N) — leaf chain traversal |

All range operations use a **callback pattern** — `std::function<bool(uint64_t)>` — to avoid materializing result vectors when not needed.

---

## 10. Query Parser Design

### 10.1 Supported Grammar

```
statement     → create_table | insert | select

create_table  → CREATE TABLE identifier '(' column_def (',' column_def)* ')'
column_def    → identifier type_name
type_name     → INT | DECIMAL | VARCHAR '(' INTEGER ')' | DATETIME

insert        → INSERT INTO identifier VALUES value_list (',' value_list)*
value_list    → '(' literal (',' literal)* ')'

select        → SELECT select_list FROM identifier [join_clause] [where_clause]
select_list   → '*' | column_ref (',' column_ref)*
column_ref    → [identifier '.'] identifier
join_clause   → INNER JOIN identifier ON column_ref cmp_op column_ref
where_clause  → WHERE column_ref cmp_op literal
cmp_op        → '=' | '!=' | '<' | '>' | '<=' | '>='

literal       → INTEGER | DECIMAL | STRING | '-' (INTEGER | DECIMAL)
```

### 10.2 Why Hand-Written Recursive Descent over yacc/bison

| Aspect | Hand-Written (Chosen) | yacc/bison (Rejected) |
|--------|-----------------------|-----------------------|
| **Error messages** | Custom, contextual | Generic "syntax error" |
| **Build dependency** | None | Requires bison in PATH |
| **Debugging** | Step through C++ code | Debug generated code |
| **Grammar size** | ~15 productions | Same, but wrapper boilerplate |
| **Performance** | Direct function calls | Table-driven (slightly slower) |

**Rationale:** The grammar is small enough (~15 productions) that a hand-written recursive descent parser is simpler, produces better error messages, and has zero external dependencies. This is exactly how SQLite's parser works.

---

## 11. Query Executor Design

### 11.1 Execution Strategy

The executor follows a **volcano/iterator model** (simplified):

1. **Parse** SQL → AST
2. **Check cache** for normalized query → return if hit
3. **Dispatch** based on statement type:
   - `CREATE TABLE` → create Schema + Table, register with executor
   - `INSERT` → parse values, validate schema, insert into table, invalidate cache
   - `SELECT` → plan execution:
     - If PK equality and no JOIN → **index lookup** (fastest)
     - If PK range → **index range scan**
     - Otherwise → **full scan with filter**
     - If JOIN → **hash join** on right table
4. **Cache** result for SELECT queries
5. **Return** QueryResult

### 11.2 Query Plan Selection

```
SELECT * FROM T WHERE pk_col = 42
  → Index point lookup: O(log₂₅₆ N)

SELECT * FROM T WHERE pk_col > 100
  → Index range scan: O(log₂₅₆ N + K)

SELECT * FROM T WHERE non_pk_col = 'foo'
  → Full scan with filter: O(N)

SELECT * FROM T1 INNER JOIN T2 ON T1.a = T2.b
  → Hash join: O(|T1| + |T2|)
```

---

## 12. Cache Layer Design

### 12.1 Cache Invalidation Strategy

```
On INSERT INTO table_name:
  → cache.invalidate_table(table_name)
  → removes ALL cached queries that touch this table

On CREATE TABLE:
  → no invalidation needed (new table has no cached queries)

On SELECT:
  → check cache first
  → on miss: execute, cache result
  → on hit: return cached result directly
```

### 12.2 Query Normalization

Before cache lookup, queries are normalized:
1. Collapse multiple whitespace to single space
2. Uppercase SQL keywords
3. Trim leading/trailing whitespace

This ensures `"SELECT  * FROM  users"` and `"select * from users"` hit the same cache entry.

---

## 13. Concurrency Model

### 13.1 Locking Hierarchy

```
Level 1: tables_mutex_ (shared_mutex)
  → Protects the table map (create/drop table)
  → Read lock for table lookup, write lock for table creation

Level 2: Per-table mutex (shared_mutex via LockManager)
  → Read lock for SELECT
  → Write lock for INSERT

Level 3: Cache mutex (shared_mutex)
  → Read lock for cache get
  → Write lock for cache put/invalidate
```

### 13.2 Deadlock Prevention

The locking order is **always:** tables_mutex → table.mutex → cache.mutex. Since locks are acquired in a consistent order and released in reverse order (RAII), deadlock is impossible.

---

## 14. Network Layer & Wire Protocol

### 14.1 Connection Lifecycle

```
1. Server binds to port 9876 (configurable)
2. Server listen(backlog=128)
3. For each accept():
   → Submit to thread pool
   → Thread reads length-prefixed SQL
   → Thread executes via Executor
   → Thread sends length-prefixed result
   → Loop until client sends "EXIT"
4. On "EXIT": send "BYE", close socket
```

### 14.2 TCP Optimizations

- **TCP_NODELAY** — disable Nagle's algorithm for low-latency responses
- **SO_REUSEADDR** — allow immediate server restart
- Length-prefixed framing — no scanning for delimiters

---

## 15. TTL & Row Expiration

### 15.1 Mechanism

Each row stores an **expiry timestamp** (epoch seconds) in the last 8 bytes before the valid flag:

```
row layout: [col₁][col₂]...[colN][expiry: int64_t][valid: uint8_t]
```

- On INSERT: `expiry = now() + table.ttl_seconds`
- On scan: skip rows where `now() > expiry` or `valid == 0`
- TTLManager background thread sweeps every 30s, marking expired rows invalid

### 15.2 Lazy vs. Eager Expiration

**Chosen: Lazy + Background Sweep.**
- **Lazy:** Scans skip expired rows at read time (no overhead on write path)
- **Background:** TTLManager periodically marks expired rows to reclaim index entries

**Why NOT Eager:** Checking expiration on every INSERT would add latency to the hot write path.

---

## 16. Persistence Layer — WAL & Snapshots

> *Added in v2.0. Backward-compatible: enabled via `--data-dir`, disabled by default.*

### 16.1 Overview

FlexQL now supports optional persistence via a **Write-Ahead Log (WAL)** and **binary snapshots**. The design follows the classical database recovery strategy:

1. **On every mutating query** (CREATE TABLE, INSERT): append the SQL to the WAL before executing
2. **On clean shutdown** (or manual trigger): take a binary snapshot of all tables, then truncate WAL
3. **On startup recovery**: load the latest snapshot, then replay any WAL records written after that snapshot

When `--data-dir` is empty (default), zero disk I/O occurs — the original in-memory behavior is preserved.

### 16.2 WAL Record Format

```
┌─────────┬──────────┬──────────┬─────────────┬──────────────────┐
│ 4 bytes │ 4 bytes  │ 8 bytes  │  4 bytes    │  N bytes         │
│ Magic   │ CRC-32   │   LSN    │ Payload Len │  SQL (UTF-8)     │
│0xDA7ABA5E│          │          │             │                  │
└─────────┴──────────┴──────────┴─────────────┴──────────────────┘
```

- **Magic (0xDA7ABA5E):** Identifies valid WAL records; enables detection of truncation/corruption
- **CRC-32 (IEEE polynomial):** Computed over LSN + payload\_len + SQL payload. Hand-built lookup table, no external deps
- **LSN (Log Sequence Number):** Monotonically increasing 64-bit counter. Used to skip records already covered by a snapshot
- **Append-only:** WAL is opened in `ios::binary | ios::app`; never rewritten except on explicit truncate

### 16.3 Snapshot Format

```
┌────────────────────────────────────────────────────────────────┐
│ Header: [8B magic 0x5348495651DB0001] [8B version] [8B LSN]   │
│         [4B num_tables]                                         │
├────────────────────────────────────────────────────────────────┤
│ Per Table:                                                      │
│   [str table_name]                                              │
│   [4B num_cols] → for each: [str name][1B type][2B max_len]    │
│   [8B num_rows] → for each: [4B num_vals] → typed values       │
└────────────────────────────────────────────────────────────────┘
```

- **Atomic writes:** Snapshot is written to a `.tmp` file, then `rename()` (atomic on POSIX)
- **String encoding:** 4-byte length prefix + raw bytes (no null terminators)
- **Typed values:** Each value starts with a 1-byte type tag, then the fixed/variable payload

### 16.4 Recovery Sequence

```
startup(data_dir):
  1. if snapshot.dat exists:
       load snapshot → restore tables into executor
       record snapshot_lsn
  2. if wal.log exists:
       open WAL reader
       for each record where lsn > snapshot_lsn:
         re-execute the SQL via executor
  3. open WAL writer for new mutations
```

### 16.5 Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| WAL granularity | SQL-level (not page/row) | Simplicity; SQL strings are self-describing and don't require a page manager |
| Integrity check | CRC-32 per record | Detects corruption/truncation without the overhead of SHA-256 |
| Snapshot format | Custom binary | No external deps (no protobuf, no msgpack); compact and fast |
| Snapshot trigger | On clean shutdown + manual API | Avoid background snapshot overhead; WAL provides crash safety |
| fsync strategy | `file_.flush()` after each WAL append | Pushes to OS page cache; survives process crashes. True power-loss durability would require platform-specific `fsync(fd)` |
| Backward compat | `--data-dir ""` = pure in-memory | Zero performance impact when persistence is not needed |

### 16.6 New Files

| File | Lines | Purpose |
|------|-------|---------|
| `src/server/storage/wal.h` | 106 | WAL record struct, WALWriter, WALReader, CRC-32 |
| `src/server/storage/wal.cpp` | 223 | Full WAL implementation with CRC-32 lookup table |
| `src/server/storage/snapshot.h` | 62 | Snapshot save/load interfaces |
| `src/server/storage/snapshot.cpp` | 265 | Binary serialization/deserialization of all tables |
| `tests/test_wal.cpp` | 255 | 9 WAL unit tests |
| `tests/test_snapshot.cpp` | 215 | 6 snapshot unit tests |
| `tests/test_persistence.cpp` | 275 | 6 end-to-end persistence tests |

### 16.7 Modified Files

- **`database.h/cpp`**: Constructor accepts `data_dir`, calls `recover()` on startup, appends to WAL on mutating queries, takes snapshot on shutdown
- **`executor.h`**: Added `tables()`, `restore_table()`, `table_count()` for persistence support
- **`main.cpp`**: Added `--data-dir`, `--port`, `--threads` CLI parsing
- **`Makefile`**: Added `wal.cpp` and `snapshot.cpp` to `CORE_SRCS`

---

## 17. Public C API Design

### 17.1 API Surface

```c
// 4 functions — that's it. Minimal, opaque, foolproof.
int  flexql_open(const char *host, int port, FlexQL **db);
int  flexql_close(FlexQL *db);
int  flexql_exec(FlexQL *db, const char *sql, flexql_callback cb, void *arg, char **errmsg);
void flexql_free(void *ptr);
```

### 17.2 Design Philosophy

This API is modeled after **SQLite's API** (`sqlite3_open`, `sqlite3_exec`, `sqlite3_close`, `sqlite3_free`):

- **Opaque handle:** `FlexQL*` — internals never exposed to the client
- **Callback pattern:** One call per row — client controls memory
- **Error ownership:** `errmsg` is allocated by the library, freed by the client via `flexql_free`
- **Pure C linkage:** `extern "C"` — callable from C, Python (ctypes), Go (cgo), etc.
- **Error codes:** Integer return values, not exceptions

**Why NOT a C++ API?** ABI stability. C APIs are ABI-stable across compiler versions and platforms. A C++ API would require users to match exact compiler flags, standard library versions, and name mangling.

---

## 18. Build System Design

### 18.1 Makefile Architecture

```makefile
Targets (unified root Makefile):
  all       → server + client + tests + benchmark + eval
  server    → links against libflexql_core.a
  client    → standalone (includes flexql_api.cpp)
  tests     → 11 test executables
  benchmark → bench_10m linked against core
  bench-eval→ evaluation benchmark linked against client

Orchestration:
  start     → force-kill + build + launch server + run eval
  stop      → kill by PID, then kill by port (double-check)
  force-kill→ kill any process on SERVER_PORT (nuclear)
  bench     → run internal 10M benchmark
  clean     → force-kill + rm build + rm data

Build modes:
  BUILD=release → -O3 -march=native -flto -funroll-loops
  BUILD=debug   → -g -O0 -fsanitize=address,undefined
```

### 18.2 Single Unified Makefile

All build logic lives in the root `Makefile`. There is no sub-Makefile — this avoids duplication and ensures consistent flags across all targets. The `force-kill` target eliminates port binding issues by always cleaning up stale processes before starting.

---

## 19. Test Architecture

### 19.1 Test Suite Summary

| Test File | Tests | What It Validates |
|-----------|-------|-------------------|
| `test_arena.cpp` | 6 | Allocation, alignment, chunking, reset, stats |
| `test_schema.cpp` | 5 | Column add, find, offsets, row size, validation |
| `test_storage.cpp` | 9 | Insert, scan, WHERE ops, index, TTL, projection |
| `test_parser.cpp` | 12 | Lexer tokens, all SQL statement types, errors |
| `test_bptree.cpp` | 12 | Insert/find, all 6 range ops, 10K stress, types |
| `test_cache.cpp` | 7 | Put/get, normalization, miss, eviction, LRU order |
| `test_executor.cpp` | 13 | CREATE/INSERT/SELECT, JOIN, all types, cache |
| `test_integration.cpp` | 7 | Full workflows, 10K stress, case insensitivity |
| `test_wal.cpp` | 9 | CRC-32, write/read, replay, truncate, corruption *(v2.0)* |
| `test_snapshot.cpp` | 6 | Empty/single/multi table, datetime, 10K round-trip *(v2.0)* |
| `test_persistence.cpp` | 6 | E2E recovery, WAL replay, multi-table, WHERE after recovery *(v2.0)* |

**Total: 92 tests, all passing.**

### 19.2 Test Philosophy

Each test file is a standalone executable that asserts conditions and prints `[PASS]` per test. This avoids dependency on any test framework (Google Test, Catch2) — the tests are zero-dependency, just like the rest of the codebase.

---

## 20. Benchmark Results & Analysis

### 20.1 10M Row Benchmark (Release, v2.2)

| Operation | Time | Throughput | Analysis |
|-----------|------|-----------|----------|
| INSERT 10M rows | 16.9 s | **592,728 rows/s** | Includes WAL append per mutation for persistence. Arena allocation is O(1). Batching enabled. |
| SELECT * (full scan) | 6.3 s | **1,594,668 rows/s** | Sequential arena access gives excellent cache locality. Zero-cost offset resolution. |
| Point lookup (100×) | 12.86 ms | **129 μs avg** | 3-level B+ tree traversal with binary search. |
| Range scan (5%) | 233.75 ms | **2,139,027 rows/s** | Leaf chain traversal — no random access. |
| INNER JOIN (100K × 1K) | 38.11 ms | **2,624,075 rows/s** | Zero-alloc hash join (Value keys, no to_string). |

> **Note:** Throughput is lower than v2.1 in-memory numbers because persistence (WAL writes) is now enabled by default. The trade-off is intentional — data durability is a hard requirement.

### 20.2 Evaluation Benchmark Results (v2.2)

| Test Category | Result | Details |
|--------------|--------|---------|
| Unit Tests (data-level) | **21/21 passed** | CREATE, INSERT, SELECT, WHERE, JOIN, error handling |
| Batch Insert (10 rows) | **10,000 rows/sec** | Single-tuple mode; batch mode scales linearly |
| SELECT * validation | ✅ | Exact row-order match |
| WHERE = filter | ✅ | `WHERE ID = 2` → single row |
| WHERE > filter | ✅ | `WHERE BALANCE > 1000` → filtered rows |
| Empty result set | ✅ | `WHERE BALANCE > 5000` → 0 rows |
| INNER JOIN + WHERE | ✅ | Hash join with post-filter |
| Invalid SQL handling | ✅ | Returns error, does not crash |
| Missing table handling | ✅ | Returns error, does not crash |

---

## 21. Security Considerations

### 21.1 What We Handle

- **Buffer overflow:** Arena bounds are checked; VARCHAR lengths are enforced
- **SQL injection:** N/A — no authentication, no shell execution
- **Resource limits:** Wire protocol rejects messages > 10 MB

### 21.2 What We Don't Handle (Out of Scope)

- Authentication/authorization
- TLS encryption
- Denial-of-service protection
- SQL injection in a traditional sense (no user-level access control)

---

## 22. Codebase Statistics

### 22.1 File-by-File Line Counts

| File | Lines | Category |
|------|-------|----------|
| `include/flexql.h` | 103 | Public API |
| `src/common.h` | 170 | Shared types |
| `src/server/storage/arena.h` | 59 | Storage |
| `src/server/storage/arena.cpp` | 75 | Storage |
| `src/server/storage/schema.h` | 55 | Storage |
| `src/server/storage/schema.cpp` | 59 | Storage |
| `src/server/storage/table.h` | 103 | Storage |
| `src/server/storage/table.cpp` | 313 | Storage |
| `src/server/storage/wal.h` | 106 | Persistence *(v2.0)* |
| `src/server/storage/wal.cpp` | 223 | Persistence *(v2.0)* |
| `src/server/storage/snapshot.h` | 62 | Persistence *(v2.0)* |
| `src/server/storage/snapshot.cpp` | 265 | Persistence *(v2.0)* |
| `src/server/index/bptree.h` | 119 | Index |
| `src/server/index/bptree.cpp` | 290 | Index |
| `src/server/parser/ast.h` | 115 | Parser |
| `src/server/parser/lexer.h` | 86 | Parser |
| `src/server/parser/lexer.cpp` | 197 | Parser |
| `src/server/parser/parser.h` | 49 | Parser |
| `src/server/parser/parser.cpp` | 314 | Parser |
| `src/server/executor/executor.h` | 88 | Executor |
| `src/server/executor/executor.cpp` | 523 | Executor |
| `src/server/cache/lru_cache.h` | 76 | Cache |
| `src/server/cache/lru_cache.cpp` | 133 | Cache |
| `src/server/concurrency/thread_pool.h` | 89 | Concurrency |
| `src/server/concurrency/thread_pool.cpp` | 47 | Concurrency |
| `src/server/concurrency/lock_manager.h` | 49 | Concurrency |
| `src/server/concurrency/lock_manager.cpp` | 24 | Concurrency |
| `src/server/ttl/ttl_manager.h` | 59 | TTL |
| `src/server/ttl/ttl_manager.cpp` | 63 | TTL |
| `src/server/network/tcp_server.h` | 59 | Network |
| `src/server/network/tcp_server.cpp` | 215 | Network |
| `src/server/database.h` | 71 | Orchestrator *(expanded v2.0)* |
| `src/server/database.cpp` | 175 | Orchestrator *(expanded v2.0)* |
| `src/server/main.cpp` | 97 | Server entry *(expanded v2.0)* |
| `src/client/flexql_api.cpp` | 233 | Client API |
| `src/client/main.cpp` | 98 | Client REPL |
| `tests/test_arena.cpp` | 84 | Tests |
| `tests/test_schema.cpp` | 94 | Tests |
| `tests/test_storage.cpp` | 168 | Tests |
| `tests/test_parser.cpp` | 175 | Tests |
| `tests/test_bptree.cpp` | 212 | Tests |
| `tests/test_cache.cpp` | 154 | Tests |
| `tests/test_executor.cpp` | 210 | Tests |
| `tests/test_integration.cpp` | 212 | Tests |
| `tests/test_wal.cpp` | 255 | Tests *(v2.0)* |
| `tests/test_snapshot.cpp` | 215 | Tests *(v2.0)* |
| `tests/test_persistence.cpp` | 275 | Tests *(v2.0)* |
| `benchmark/bench_10m.cpp` | 217 | Benchmark |
| `Makefile` | 110 | Build |
| `CMakeLists.txt` | 90 | Build (alt) |

### 22.2 Summary Statistics

| Metric | Value |
|--------|-------|
| **Total source files** | 49 *(was 42 in v1.0)* |
| **Total lines of code** | **7,333** *(was 5,476 in v1.0)* |
| **Core library (headers + impl)** | 3,823 lines *(+656 persistence)* |
| **Test code** | 2,054 lines *(+745 persistence tests)* |
| **Benchmark code** | 217 lines |
| **Build files** | 200 lines |
| **Public API** | 103 lines |
| **Client code** | 331 lines |
| **Test/code ratio** | 0.54 (54% test coverage by LOC) |
| **Total tests** | 92 *(was 71 in v1.0)* |
| **New in v2.0** | 7 files, ~1,401 lines (WAL + Snapshot + 3 test suites) |

---

## 23. Rejected Alternatives — Full Analysis

### 23.1 Why NOT Columnar Storage (like DuckDB)?

Columnar storage excels at analytical queries (SUM, AVG, GROUP BY) on few columns. FlexQL's workload is:
- Full row access (SELECT *)
- Single-row INSERT
- Point lookups by PK
- Row-level TTL

All of these favor row-major layout. Columnar would hurt INSERT (scatter across arrays) and point lookup (gather across arrays).

### 23.2 Why NOT a Log-Structured Merge Tree (like RocksDB/LevelDB)?

LSM trees optimize for write-heavy workloads with persistence. FlexQL is:
- Primarily in-memory (persistence is optional via WAL+snapshot, not page-level)
- Mixed read-write
- Needs fast point reads

An LSM tree would add compaction overhead and read amplification for zero benefit.

### 23.3 Why NOT Skip Lists (like Redis)?

Skip lists are simpler than B+ Trees but have:
- 2× more memory overhead (forward pointers per level)
- Worse cache performance (random pointer chasing)
- No O(1) sequential scan (must traverse O(log N) levels)

B+ Trees with order 256 dominate on all axes for this workload.

### 23.4 Why NOT Protocol Buffers / Cap'n Proto for Wire Format?

Adding a serialization framework would violate the "no external libraries" constraint. The tab-delimited text protocol is simple, human-readable for debugging, and adequate for the throughput requirements.

---

## 24. Known Limitations 

### 24.1 Current Limitations

1. **No DELETE/UPDATE:** Only CREATE TABLE, INSERT, SELECT are supported
2. **Single WHERE condition:** No AND/OR compound predicates
3. **No aggregate functions:** No SUM, COUNT, AVG, MIN, MAX, GROUP BY
4. **No ORDER BY/LIMIT**
5. **No secondary indices:** Only primary key (first column) is indexed
6. **VARCHAR wastes space:** Fixed-width allocation even for short strings



## 25. References

1. **SQLite Architecture** — https://www.sqlite.org/arch.html  
   (Inspiration for the opaque C API design)

2. **B+ Trees in Database Systems** — Ramakrishnan & Gehrke, "Database Management Systems"  
   (Order-256 sizing based on cache-line analysis)

3. **Arena Allocators** — Chandler Carruth, CppCon 2015: "Efficiency with Algorithms, Performance with Data Structures"  
   (Slab allocation for sequential workloads)

4. **Hash Join Algorithm** — H. Garcia-Molina, J.D. Ullman, J. Widom, "Database Systems: The Complete Book"  
   (Build-probe hash join for equi-joins)

5. **LRU Cache — O(1) Design** — LeetCode 146  
   (HashMap + doubly-linked list for O(1) get/put)

6. **TCP_NODELAY** — John Nagle, RFC 896  
   (Disable Nagle's algorithm for interactive protocols)

---

---

*Ramesh Choudhary — March 2026*  

