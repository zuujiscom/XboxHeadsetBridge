CC      := clang
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -std=c11
LDFLAGS := -framework CoreFoundation -framework IOKit
BUILD   := build

SWIFTC  := swiftc
SWIFTFLAGS := -O -parse-as-library

.PHONY: all clean probe
all: $(BUILD)/gip-probe $(BUILD)/gip-tone $(BUILD)/gip-mic $(BUILD)/gip-bridge $(BUILD)/gip-status plugin-bundle menubar-app

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

# --- menu bar app -----------------------------------------------------------
#
# A LSUIElement SwiftUI app that starts and stops gip-bridge and shows the
# headset's battery, mic and volume state from the shared ring. The daemon is
# copied inside the bundle so the app is self-contained.

MENUBAR      := $(BUILD)/XboxHeadsetMenu.app
MENUBAR_BIN  := $(MENUBAR)/Contents/MacOS/XboxHeadsetMenu
MENUBAR_SRC  := $(wildcard menubar/*.swift)

.PHONY: menubar-app install-menubar
menubar-app: $(MENUBAR_BIN) $(MENUBAR)/Contents/Resources/gip-bridge

$(BUILD)/ringshim.o: menubar/ringshim.c menubar/ringshim.h shared/ring.h | $(BUILD)
	$(CC) $(CFLAGS) -c menubar/ringshim.c -o $@

$(MENUBAR_BIN): $(MENUBAR_SRC) menubar/Bridging.h menubar/Info.plist $(BUILD)/ringshim.o
	@mkdir -p $(MENUBAR)/Contents/MacOS $(MENUBAR)/Contents/Resources
	cp menubar/Info.plist $(MENUBAR)/Contents/Info.plist
	$(SWIFTC) $(SWIFTFLAGS) -import-objc-header menubar/Bridging.h \
		$(MENUBAR_SRC) $(BUILD)/ringshim.o -o $@
	@codesign --force --sign - $(MENUBAR) 2>/dev/null || true

$(MENUBAR)/Contents/Resources/gip-bridge: $(BUILD)/gip-bridge
	@mkdir -p $(MENUBAR)/Contents/Resources
	cp $(BUILD)/gip-bridge $@

# Drop the app into /Applications so it can be added to Login Items.
install-menubar: menubar-app
	rm -rf /Applications/XboxHeadsetMenu.app
	cp -R $(MENUBAR) /Applications/

$(BUILD):
	@mkdir -p $(BUILD)

probe: $(BUILD)/gip-probe
	./$(BUILD)/gip-probe

clean:
	rm -rf $(BUILD)
