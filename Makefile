CC      := clang
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -std=c11
LDFLAGS := -framework CoreFoundation -framework IOKit
BUILD   := build

.PHONY: all clean probe
all: $(BUILD)/gip-probe $(BUILD)/gip-tone $(BUILD)/gip-mic $(BUILD)/gip-bridge $(BUILD)/gip-status plugin-bundle

$(BUILD)/gip-probe: src/probe.c src/gipusb.c src/gip.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/gip-tone: src/tone.c src/gipusb.c src/gip.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/gip-mic: src/mic.c src/gipusb.c src/gip.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/gip-bridge: src/bridge.c src/gip_auth.c src/gipusb.c src/gip.c shared/ring.h | $(BUILD)
	$(CC) $(CFLAGS) -I/opt/homebrew/opt/openssl@3/include src/bridge.c src/gip_auth.c src/gipusb.c src/gip.c -o $@ $(LDFLAGS) -L/opt/homebrew/opt/openssl@3/lib -lcrypto

$(BUILD)/gip-status: src/status.c shared/ring.h | $(BUILD)
	$(CC) $(CFLAGS) src/status.c -o $@ $(LDFLAGS)

DRIVER := $(BUILD)/XboxHeadset.driver
PLUGIN_DST := /Library/Audio/Plug-Ins/HAL

.PHONY: plugin-bundle install-plugin uninstall-plugin
plugin-bundle: $(DRIVER)/Contents/MacOS/XboxHeadset

$(DRIVER)/Contents/MacOS/XboxHeadset: plugin/XboxHeadset.c plugin/Info.plist shared/ring.h
	@mkdir -p $(DRIVER)/Contents/MacOS
	cp plugin/Info.plist $(DRIVER)/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle plugin/XboxHeadset.c -o $@ \
		-framework CoreFoundation -framework CoreAudio

install-plugin: plugin-bundle
	sudo rm -rf $(PLUGIN_DST)/XboxHeadset.driver
	sudo cp -R $(DRIVER) $(PLUGIN_DST)/
	sudo chown -R root:wheel $(PLUGIN_DST)/XboxHeadset.driver
	sudo killall coreaudiod

uninstall-plugin:
	sudo rm -rf $(PLUGIN_DST)/XboxHeadset.driver
	sudo killall coreaudiod

$(BUILD):
	@mkdir -p $(BUILD)

probe: $(BUILD)/gip-probe
	./$(BUILD)/gip-probe

clean:
	rm -rf $(BUILD)
