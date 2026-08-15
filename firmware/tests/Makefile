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
LINK  := $(BUILD)/test_link
LAMP  := $(BUILD)/test_lamp
BTN   := $(BUILD)/test_button
WDOG  := $(BUILD)/test_wdog
CMD   := $(BUILD)/test_cmd
LEDIT := $(BUILD)/test_lineedit
SET   := $(BUILD)/test_settings
TRANS := $(BUILD)/test_transport
SIM   := $(BUILD)/at_host
SRCS  := test_rnwf_at.c ../src/rnwf_at.c
DEPS  := ../src/rnwf_at.h ../src/rnwf_at_cmds.h

.PHONY: all link lamp button wdog cmd lineedit settings transport sim clean
all: $(BIN) $(LINK) $(LAMP) $(BTN) $(WDOG) $(CMD) $(LEDIT) $(SET) $(TRANS)
	./$(BIN)
	./$(LINK)
	./$(LAMP)
	./$(BTN)
	./$(WDOG)
	./$(CMD)
	./$(LEDIT)
	./$(SET)
	./$(TRANS)

$(BIN): $(SRCS) $(DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

transport: $(TRANS)
	./$(TRANS)
$(TRANS): test_transport.c ../src/transport.c ../src/transport.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_transport.c ../src/transport.c $(LDFLAGS)

settings: $(SET)
	./$(SET)
$(SET): test_settings.c ../src/settings.c ../src/settings.h ../src/cmd.c ../src/lamp.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_settings.c ../src/settings.c ../src/cmd.c ../src/lamp.c $(LDFLAGS)

lineedit: $(LEDIT)
	./$(LEDIT)
$(LEDIT): test_lineedit.c ../src/lineedit.c ../src/lineedit.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_lineedit.c ../src/lineedit.c $(LDFLAGS)

cmd: $(CMD)
	./$(CMD)
$(CMD): test_cmd.c ../src/cmd.c ../src/cmd.h ../src/lamp.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cmd.c ../src/cmd.c ../src/lamp.c $(LDFLAGS)

wdog: $(WDOG)
	./$(WDOG)
$(WDOG): test_wdog.c ../src/wdog.c ../src/wdog.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_wdog.c ../src/wdog.c $(LDFLAGS)

button: $(BTN)
	./$(BTN)
$(BTN): test_button.c ../src/button.c ../src/button.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_button.c ../src/button.c $(LDFLAGS)

lamp: $(LAMP)
	./$(LAMP)
$(LAMP): test_lamp.c ../src/lamp.c ../src/lamp.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_lamp.c ../src/lamp.c $(LDFLAGS)

link: $(LINK)
	./$(LINK)
$(LINK): test_link.c ../src/link.c ../src/link.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_link.c ../src/link.c $(LDFLAGS)

# The host runner drives the real rnwf_at.c against sim/fake_rnwf02.py over a PTY (ADR-0015).
sim: $(SIM)
$(SIM): ../sim/at_host.c ../src/rnwf_at.c $(DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../sim/at_host.c ../src/rnwf_at.c $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
