CC       := gcc
CPPFLAGS := -Iinclude -D_GNU_SOURCE -MMD -MP
CFLAGS   := -std=c11 -Wall -Wextra -g -pthread $(SAN)
LDFLAGS  := $(SAN)
LDLIBS   := -pthread
BUILD    ?= build

# Core library: shared helpers (and the schedulers from D4 on).
CORE_SRC := $(wildcard src/common/*.c)
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
CORE_LIB := $(BUILD)/liblbcore.a

# lb = master + worker; main.c holds main(), the rest is reused by the tests.
LB_MAIN  := src/master/main.c
LB_SRC   := $(filter-out $(LB_MAIN),$(wildcard src/master/*.c src/worker/*.c))
LB_OBJ   := $(LB_SRC:%.c=$(BUILD)/%.o)
MAIN_OBJ := $(LB_MAIN:%.c=$(BUILD)/%.o)

LBCTL_OBJ := $(BUILD)/tools/lbctl.o

TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(TEST_SRC:%.c=$(BUILD)/%)

BACKEND_SRC := $(wildcard tests/backends/*.c)
BACKEND_BIN := $(BACKEND_SRC:%.c=$(BUILD)/%)

.PHONY: all clean test asan backends

# Keep test and backend objects (chained pattern rules would delete them).
.SECONDARY:

all: lb lbctl

$(CORE_LIB): $(CORE_OBJ)
	ar rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Link order: objects first, then the library, then system libraries.
lb: $(MAIN_OBJ) $(LB_OBJ) $(CORE_LIB)
	$(CC) $(LDFLAGS) $(MAIN_OBJ) $(LB_OBJ) $(CORE_LIB) $(LDLIBS) -o $@

lbctl: $(LBCTL_OBJ) $(CORE_LIB)
	$(CC) $(LDFLAGS) $(LBCTL_OBJ) $(CORE_LIB) $(LDLIBS) -o $@

$(BUILD)/tests/unit/%: $(BUILD)/tests/unit/%.o $(LB_OBJ) $(CORE_LIB)
	$(CC) $(LDFLAGS) $< $(LB_OBJ) $(CORE_LIB) $(LDLIBS) -o $@

$(BUILD)/tests/backends/%: $(BUILD)/tests/backends/%.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $< $(CORE_LIB) $(LDLIBS) -o $@

# Each test is a program: exit status 0 = pass. Stop on the first failure.
test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "RUN  $$t"; ./$$t; done; echo "all tests passed"

backends: $(BACKEND_BIN)

asan:
	$(MAKE) BUILD=build-asan SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	rm -rf build build-asan lb lbctl

-include $(CORE_OBJ:.o=.d) $(LB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(LBCTL_OBJ:.o=.d) \
         $(TEST_BIN:=.d) $(BACKEND_BIN:=.d)
