CC      := clang
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -std=c11
LDFLAGS := -framework CoreFoundation -framework IOKit
BUILD   := build

.PHONY: all clean probe
all: $(BUILD)/gip-probe

$(BUILD)/gip-probe: src/probe.c src/gipusb.c src/gip.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)

probe: $(BUILD)/gip-probe
	./$(BUILD)/gip-probe

clean:
	rm -rf $(BUILD)
