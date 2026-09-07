include ../desktop-shared/Makefile.sources

SOURCES_MM += $(DESKTOP_SRC_DIR)/nfd/nfd_cocoa.mm
LDFLAGS += -framework AppKit -framework UniformTypeIdentifiers

include ../desktop-shared/Makefile.common

APP_NAME=GearSF7000
SDL3_DYLIB := $(shell pkg-config --variable=libdir sdl3)/libSDL3.0.dylib
SDL3_INSTALL_NAME := $(shell otool -L $(TARGET) | awk '/libSDL3\.0\.dylib/{print $$1; exit}')

.PHONY: bundle dist mcp

# Developer build with the MCP server compiled in. Plain `make` leaves MCP out
# on purpose (see ENABLE_MCP in Makefile.common), and a binary built without it
# does not recognise --mcp-http: it prints the usage text and exits. This target
# keeps the MCP build a single command so that failure mode cannot happen by
# forgetting a flag.
mcp:
	$(MAKE) ENABLE_MCP=1 all
	$(MAKE) ENABLE_MCP=1 bundle
	@echo "MCP build ready: open -n $(APP_NAME).app --args --mcp-http"

# .mcp.json and the Finder both launch the .app, never the bare binary, so a
# bundle left behind by an earlier build is a binary you did not make being run
# as though you had. It looks like a change that did not take effect. `make`
# cannot rebuild the bundle on its own - it would overwrite the executable of a
# running instance - so it says so instead, and the About box now carries the
# compile time to settle it either way.
.PHONY: bundle_staleness_check
# After the link, not before it: the comparison is against the binary this
# build just produced, and the message is worth more as the last thing printed.
bundle_staleness_check: $(TARGET)
	@if [ "$(SKIP_BUNDLE_CHECK)" != "1" ] && \
	    [ -f $(APP_NAME).app/Contents/MacOS/$(TARGET) ] && \
	    [ $(TARGET) -nt $(APP_NAME).app/Contents/MacOS/$(TARGET) ]; then \
	    echo ""; \
	    echo "  NOTE: $(APP_NAME).app is older than the binary just built."; \
	    echo "        Anything launching the .app - including MCP clients -"; \
	    echo "        still gets the old one. Run 'make bundle' (or 'make mcp')"; \
	    echo "        to update it, but not while an instance is running."; \
	    echo ""; \
	fi

all: bundle_staleness_check

bundle:
	rm -rf $(APP_NAME).app
	mkdir -p $(APP_NAME).app/Contents/{MacOS,Resources,Frameworks}
	cp Info.plist $(APP_NAME).app/Contents/Info.plist
	cp iconfile.icns $(APP_NAME).app/Contents/Resources/
	cp ../desktop-shared/imgui/fonts/Iosevka/GearSF7000Mono-Condensed.ttf $(APP_NAME).app/Contents/Resources/
	cp ../desktop-shared/imgui/fonts/Iosevka/GearSF7000Mono-CondensedLight.ttf $(APP_NAME).app/Contents/Resources/
	cp ../desktop-shared/assets/sc3000-keyboard-compact.png $(APP_NAME).app/Contents/Resources/
	cp $(TARGET) $(APP_NAME).app/Contents/MacOS/
	cp ../gamecontrollerdb.txt $(APP_NAME).app/Contents/MacOS/
	cp $(SDL3_DYLIB) $(APP_NAME).app/Contents/Frameworks/
	install_name_tool -change $(SDL3_INSTALL_NAME) @executable_path/../Frameworks/libSDL3.0.dylib $(APP_NAME).app/Contents/MacOS/$(TARGET)
	# Re-sign after copying so recent macOS releases accept the rebuilt bundle.
	codesign --force --sign - --deep $(APP_NAME).app

dist:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) bundle
	@echo Success!!
