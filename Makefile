CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
LDFLAGS ?=

SRC_DIR = src
INC_DIR = include
TOOLS_DIR = tools
BUILD_DIR = build
SAMPLES_DIR = samples
TESTS_DIR = tests

CORE_SRC = $(SRC_DIR)/slitherlink_core.c
CORE_HDR = $(INC_DIR)/slitherlink_core.h
CPPFLAGS ?= -I$(INC_DIR)

.PHONY: all clean check

all: $(BUILD_DIR)/slitherlink_cli $(BUILD_DIR)/generate_puzzle $(BUILD_DIR)/libslitherlink.dylib

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/slitherlink_cli: $(SRC_DIR)/ai-generated.c $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC_DIR)/ai-generated.c $(CORE_SRC) -o $@ $(LDFLAGS)

$(BUILD_DIR)/generate_puzzle: $(TOOLS_DIR)/generate_puzzle.c $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TOOLS_DIR)/generate_puzzle.c $(CORE_SRC) -o $@ $(LDFLAGS)

$(BUILD_DIR)/libslitherlink.dylib: $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -dynamiclib $(CORE_SRC) -o $@ $(LDFLAGS)

check: all
	$(BUILD_DIR)/slitherlink_cli --count 2 '$(TESTS_DIR)/testcase_7*7.txt'
	$(BUILD_DIR)/generate_puzzle 7 7 2 123 > /tmp/slitherlink_generated_7x7.txt
	$(BUILD_DIR)/slitherlink_cli --count 2 /tmp/slitherlink_generated_7x7.txt
	$(BUILD_DIR)/generate_puzzle 7 7 2 123 walk > /tmp/slitherlink_generated_walk_7x7.txt
	$(BUILD_DIR)/slitherlink_cli --count 2 /tmp/slitherlink_generated_walk_7x7.txt
	$(BUILD_DIR)/generate_puzzle 7 7 2 123 growth > /tmp/slitherlink_generated_growth_7x7.txt
	$(BUILD_DIR)/slitherlink_cli --count 2 /tmp/slitherlink_generated_growth_7x7.txt

clean:
	rm -f $(BUILD_DIR)/slitherlink_cli $(BUILD_DIR)/generate_puzzle $(BUILD_DIR)/libslitherlink.dylib $(BUILD_DIR)/slitherlink_core.o $(BUILD_DIR)/ai-generated
