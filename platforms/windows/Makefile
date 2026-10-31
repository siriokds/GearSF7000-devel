ENABLE_MCP ?= 1

include ../desktop-shared/Makefile.sources

SOURCES_CXX += $(DESKTOP_SRC_DIR)/nfd/nfd_win.cpp
CPPFLAGS += -DNOMINMAX
LDFLAGS += -lole32 -luuid -lshell32 -lws2_32
WINDRES ?= windres

include ../desktop-shared/Makefile.common

# Embed keyboard/font resources used by the Win32 frontend.
WINDOWS_RESOURCE := $(OBJ_DIR)/GearSF7000-resource.o
$(TARGET): $(WINDOWS_RESOURCE)
OBJECTS += $(WINDOWS_RESOURCE)
$(WINDOWS_RESOURCE): GearSF7000.rc resource.h ../desktop-shared/assets/sc3000-keyboard-compact.png $(wildcard ../desktop-shared/imgui/fonts/Iosevka/*.ttf)
	@mkdir -p "$(@D)"
	$(WINDRES) -I. "$<" "$@"
