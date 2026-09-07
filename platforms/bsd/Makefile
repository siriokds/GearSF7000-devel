# The regular target is the complete developer/debugger build, MCP included.
# `make computer` below overrides this before the source list is evaluated.
ENABLE_MCP ?= 1

include ../desktop-shared/Makefile.sources

SOURCES_CXX += $(DESKTOP_SRC_DIR)/nfd/nfd_gtk.cpp
CPPFLAGS += `pkg-config --cflags gtk+-3.0`
LDFLAGS += `pkg-config --libs gtk+-3.0`

include ../desktop-shared/Makefile.common

# Cartridge-computer build. Keep the normal BSD target as the complete
# GearSF7000 application and opt into the reduced GearSC3000 product.
# See the Linux target for the separate product-profile object tree.
.PHONY: computer
computer:
	$(MAKE) all \
		BUILD_PROFILE=computer \
		TARGET_NAME=gearsc3000 \
		PRODUCT_SC3000=1 \
		ENABLE_DEBUG_TOOLS=0 \
		ENABLE_RECORDER=0 \
		ENABLE_MCP=0 \
		ENABLE_SPRITE_EXPANDER=0 \
		ENABLE_AY=0 \
		ENABLE_SF7000=0 \
		ENABLE_SR1000=0 \
		ENABLE_SP400=0
