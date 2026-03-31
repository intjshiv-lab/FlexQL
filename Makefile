# ─── FlexQL Unified Makefile ────────────────────────────────────────────────
# Usage:
#   make start     — Build, start server (persistent), run evaluation
#   make stop      — Stop the running server
#   make build     — Build all targets
#   make test      — Run unit tests
#   make bench     — Run internal 10M benchmark (direct, no server)
#   make clean     — Clean build artifacts

.PHONY: build start stop test bench clean force-kill all server client tests benchmark benchmark-eval run-tests

# ─── Directories ────────────────────────────────────────────────────────────
PROJECT_DIR  := flexql
SRC_DIR      := $(PROJECT_DIR)/src
INCLUDE_DIR  := $(PROJECT_DIR)/include
TESTS_DIR    := $(PROJECT_DIR)/tests
BENCH_DIR    := $(PROJECT_DIR)/benchmark
BUILD_DIR    := $(PROJECT_DIR)/build
PID_FILE     := .server.pid
DATA_DIR     := .flexql_data
SERVER_PORT  := 9000
MAX_WAIT     := 10

# ─── Compiler ───────────────────────────────────────────────────────────────
CXX        := clang++
CXXFLAGS   := -std=c++17 -Wall -Wextra -Wpedantic
INCLUDES   := -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src

# Build modes
RELEASE_FLAGS := -O3 -ffast-math -DNDEBUG -march=native -flto \
                 -fno-signed-zeros -fno-trapping-math -fmerge-all-constants \
                 -fvisibility=hidden -funroll-loops
LDFLAGS_OPT   := -flto
DEBUG_FLAGS   := -g -O0 -DDEBUG -fsanitize=address,undefined
LDFLAGS_DBG   := -fsanitize=address,undefined

BUILD ?= release
ifeq ($(BUILD),debug)
    CXXFLAGS += $(DEBUG_FLAGS)
    LDFLAGS  += $(LDFLAGS_DBG)
else
    CXXFLAGS += $(RELEASE_FLAGS)
    LDFLAGS  += $(LDFLAGS_OPT)
endif

# Platform-specific
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
    LDFLAGS += -lpthread
endif

# ─── Core Sources ───────────────────────────────────────────────────────────
CORE_SRCS := \
    $(SRC_DIR)/server/storage/arena.cpp \
    $(SRC_DIR)/server/storage/schema.cpp \
    $(SRC_DIR)/server/storage/table.cpp \
    $(SRC_DIR)/server/storage/wal.cpp \
    $(SRC_DIR)/server/storage/snapshot.cpp \
    $(SRC_DIR)/server/parser/lexer.cpp \
    $(SRC_DIR)/server/parser/parser.cpp \
    $(SRC_DIR)/server/executor/executor.cpp \
    $(SRC_DIR)/server/index/bptree.cpp \
    $(SRC_DIR)/server/cache/lru_cache.cpp \
    $(SRC_DIR)/server/concurrency/thread_pool.cpp \
    $(SRC_DIR)/server/concurrency/lock_manager.cpp \
    $(SRC_DIR)/server/ttl/ttl_manager.cpp \
    $(SRC_DIR)/server/network/tcp_server.cpp \
    $(SRC_DIR)/server/database.cpp

CORE_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# ─── Test Sources ───────────────────────────────────────────────────────────
TEST_SRCS := $(wildcard $(TESTS_DIR)/test_*.cpp)
TEST_BINS := $(patsubst $(TESTS_DIR)/%.cpp,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

# ═══════════════════════════════════════════════════════════════════════════
#  BUILD TARGETS
# ═══════════════════════════════════════════════════════════════════════════

all: server client tests benchmark benchmark-eval
build: all

# ─── Core library (static archive) ─────────────────────────────────────────
$(BUILD_DIR)/libflexql_core.a: $(CORE_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# Pattern rule for core objects
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ─── Server ─────────────────────────────────────────────────────────────────
server: $(BUILD_DIR)/flexql_server

$(BUILD_DIR)/flexql_server: $(SRC_DIR)/server/main.cpp $(BUILD_DIR)/libflexql_core.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lflexql_core $(LDFLAGS) -o $@

# ─── Client ─────────────────────────────────────────────────────────────────
client: $(BUILD_DIR)/flexql_client

$(BUILD_DIR)/flexql_client: $(SRC_DIR)/client/main.cpp $(SRC_DIR)/client/flexql_api.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ $(LDFLAGS) -o $@

# ─── Tests ──────────────────────────────────────────────────────────────────
tests: $(TEST_BINS)

$(BUILD_DIR)/tests/test_%: $(TESTS_DIR)/test_%.cpp $(BUILD_DIR)/libflexql_core.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lflexql_core $(LDFLAGS) -o $@

# ─── Internal Benchmark (10M rows, direct) ─────────────────────────────────
benchmark: $(BUILD_DIR)/flexql_bench

$(BUILD_DIR)/flexql_bench: $(BENCH_DIR)/bench_10m.cpp $(BUILD_DIR)/libflexql_core.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lflexql_core $(LDFLAGS) -o $@

# ─── Evaluation Benchmark (client-server, via TCP) ─────────────────────────
benchmark-eval: $(BUILD_DIR)/flexql_eval

$(BUILD_DIR)/flexql_eval: benchmark_flexql.cpp $(SRC_DIR)/client/flexql_api.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ $(LDFLAGS) -o $@

# ─── Run Tests ──────────────────────────────────────────────────────────────
run-tests: tests
	@echo "=== Running all tests ==="
	@for t in $(TEST_BINS); do \
		echo "\n--- $$t ---"; \
		$$t || exit 1; \
	done
	@echo "\n=== All tests passed ==="

# ═══════════════════════════════════════════════════════════════════════════
#  ORCHESTRATION TARGETS
# ═══════════════════════════════════════════════════════════════════════════

# ─── Force Kill — Nuclear option, always works ──────────────────────────────
force-kill:
	@PID=$$(lsof -ti :$(SERVER_PORT) 2>/dev/null); \
	if [ -n "$$PID" ]; then \
		echo "🔄 Cleaning up port $(SERVER_PORT) (PID $$PID)..."; \
		kill -9 $$PID 2>/dev/null || true; \
		sleep 0.5; \
	fi
	@rm -f $(PID_FILE)

# ─── Start ──────────────────────────────────────────────────────────────────
start: force-kill build
	@echo ""
	@echo "╔══════════════════════════════════════╗"
	@echo "║      Starting FlexQL Server          ║"
	@echo "╚══════════════════════════════════════╝"
	@mkdir -p $(DATA_DIR)
	@cd $(PROJECT_DIR) && ./build/flexql_server --port $(SERVER_PORT) --data-dir ../$(DATA_DIR) &
	@sleep 0.5
	@lsof -ti :$(SERVER_PORT) > $(PID_FILE) 2>/dev/null || true
	@echo "⏳ Waiting for server on port $(SERVER_PORT)..."
	@for i in $$(seq 1 $(MAX_WAIT)); do \
		if nc -z 127.0.0.1 $(SERVER_PORT) 2>/dev/null; then \
			echo "✅ Server is UP (port $(SERVER_PORT), PID $$(cat $(PID_FILE) 2>/dev/null))"; \
			break; \
		fi; \
		if [ $$i -eq $(MAX_WAIT) ]; then \
			echo "❌ Server failed to start within $(MAX_WAIT)s"; \
			$(MAKE) stop; \
			exit 1; \
		fi; \
		sleep 1; \
	done
	@echo ""
	@echo "╔══════════════════════════════════════╗"
	@echo "║      Running Evaluation Benchmark    ║"
	@echo "╚══════════════════════════════════════╝"
	@$(BUILD_DIR)/flexql_eval
	@echo ""
	@echo "✅ All done. Server is running in background."
	@echo "   Use 'make stop' to stop the server."

# ─── Stop ───────────────────────────────────────────────────────────────────
stop:
	@if [ -f $(PID_FILE) ]; then \
		PID=$$(cat $(PID_FILE)); \
		echo "🛑 Stopping FlexQL Server (PID $$PID)..."; \
		kill $$PID 2>/dev/null || true; \
		sleep 0.5; \
		kill -9 $$PID 2>/dev/null || true; \
		rm -f $(PID_FILE); \
	fi
	@PID=$$(lsof -ti :$(SERVER_PORT) 2>/dev/null); \
	if [ -n "$$PID" ]; then \
		echo "🛑 Killing orphan on port $(SERVER_PORT) (PID $$PID)..."; \
		kill -9 $$PID 2>/dev/null || true; \
	fi
	@rm -f $(PID_FILE)
	@echo "✅ Server stopped."

# ─── Bench (internal 10M benchmark, no server needed) ──────────────────────
bench: build
	@echo "Running 10M-row benchmark..."
	@$(BUILD_DIR)/flexql_bench
	@python3 flexql/update_designdoc.py

# ─── Test (unit tests) ─────────────────────────────────────────────────────
test: build run-tests

# ─── Clean ──────────────────────────────────────────────────────────────────
clean: force-kill
	@rm -rf $(BUILD_DIR)
	@rm -rf $(DATA_DIR)
	@rm -f $(PID_FILE)
	@echo "✅ Cleaned."
