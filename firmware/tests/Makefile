# Host tests for the AT client core.
#
# No Zephyr, no hardware, no west — just clang. This exists because native_sim does not work on
# macOS (ADR-0015) and because a parser deserves tests that run in milliseconds.
#
#   make            build and run
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -O1 -g \
           -fsanitize=address,undefined
LDFLAGS ?= -fsanitize=address,undefined

BUILD := build
BIN   := $(BUILD)/test_rnwf_at
SRCS  := test_rnwf_at.c ../src/rnwf_at.c

.PHONY: all clean
all: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) ../src/rnwf_at.h ../src/rnwf_at_cmds.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
