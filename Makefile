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

# The bridge core, shared by the gip-bridge CLI and the menu bar app.
SSL_PREFIX  := /opt/homebrew/opt/openssl@3
SSL_CFLAGS  := -I$(SSL_PREFIX)/include
SSL_LDFLAGS := -L$(SSL_PREFIX)/lib -lcrypto
BRIDGE_SRC  := src/bridge.c src/gip_auth.c src/gipusb.c src/gip.c
BRIDGE_HDR  := src/bridge.h src/gip.h src/gipusb.h src/gip_auth.h shared/ring.h
BRIDGE_OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(BRIDGE_SRC))

$(BUILD)/%.o: src/%.c $(BRIDGE_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -c $< -o $@

$(BUILD)/gip-bridge: src/main.c $(BRIDGE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) src/main.c $(BRIDGE_OBJ) -o $@ $(LDFLAGS) $(SSL_LDFLAGS)

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
menubar-app: $(MENUBAR_BIN)

$(BUILD)/ringshim.o: menubar/ringshim.c menubar/ringshim.h shared/ring.h | $(BUILD)
	$(CC) $(CFLAGS) -c menubar/ringshim.c -o $@

# The bridge runs inside the app, on its own thread -- no child process. The
# gip-bridge CLI is still built from the same objects for diagnosis.
#
# libcrypto is linked by absolute Homebrew path, which does not exist on other
# machines, so it is copied into the bundle and both install names rewritten to
# @rpath.
SSL_DYLIB := libcrypto.3.dylib

$(MENUBAR_BIN): $(MENUBAR_SRC) menubar/Bridging.h menubar/Info.plist $(BUILD)/ringshim.o $(BRIDGE_OBJ)
	@mkdir -p $(MENUBAR)/Contents/MacOS $(MENUBAR)/Contents/Resources $(MENUBAR)/Contents/Frameworks
	cp menubar/Info.plist $(MENUBAR)/Contents/Info.plist
	cp $(SSL_PREFIX)/lib/$(SSL_DYLIB) $(MENUBAR)/Contents/Frameworks/
	chmod u+w $(MENUBAR)/Contents/Frameworks/$(SSL_DYLIB)
	install_name_tool -id @rpath/$(SSL_DYLIB) \
		$(MENUBAR)/Contents/Frameworks/$(SSL_DYLIB)
	$(SWIFTC) $(SWIFTFLAGS) -import-objc-header menubar/Bridging.h -Isrc -Ishared \
		$(MENUBAR_SRC) $(BUILD)/ringshim.o $(BRIDGE_OBJ) -o $@ \
		$(LDFLAGS) $(SSL_LDFLAGS) -Xlinker -rpath -Xlinker @executable_path/../Frameworks
	install_name_tool -change $(SSL_PREFIX)/lib/$(SSL_DYLIB) @rpath/$(SSL_DYLIB) $@
	@codesign --force --sign - $(MENUBAR)/Contents/Frameworks/$(SSL_DYLIB) 2>/dev/null || true
	@codesign --force --sign - $(MENUBAR) 2>/dev/null || true

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
