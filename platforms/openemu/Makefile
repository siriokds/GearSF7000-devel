PROJECT_ROOT := $(abspath ../..)
SOURCE_DIR := $(PROJECT_ROOT)/src
SDK_ROOT ?= $(abspath ../../../../OpenEmu/OpenEmu-SDK)
OPENEMU_APP ?= /Applications/OpenEmu.app
FRAMEWORKS := $(OPENEMU_APP)/Contents/Frameworks

BUILD_DIR := build
BUNDLE := $(BUILD_DIR)/GearSC3000.oecoreplugin
EXECUTABLE := $(BUNDLE)/Contents/MacOS/GearSC3000
VERSION ?= 1.0.0
RELEASE_TAG ?= openemu-v$(VERSION)
ARCHIVE := $(BUILD_DIR)/GearSC3000.oecoreplugin.zip
APPCAST := $(BUILD_DIR)/gearsc3000-openemu-appcast.xml
OPENEMU_SILICON_APPCAST := $(BUILD_DIR)/gearsc3000.xml
CHECKSUM := $(ARCHIVE).sha256
APPCAST_URL ?= https://github.com/siriokds/GearSF7000-devel/releases/latest/download/gearsc3000-openemu-appcast.xml
DOWNLOAD_URL ?= https://github.com/siriokds/GearSF7000-devel/releases/download/$(RELEASE_TAG)/GearSC3000.oecoreplugin.zip
CODE_SIGN_IDENTITY ?= -
CODE_SIGN_OPTIONS := --force --sign "$(CODE_SIGN_IDENTITY)"
ifneq ($(CODE_SIGN_IDENTITY),-)
CODE_SIGN_OPTIONS += --timestamp --options runtime
endif

SOURCES_C := $(SOURCE_DIR)/miniz/miniz.c
SOURCES_CXX := $(SOURCE_DIR)/build_info.cpp \
    $(SOURCE_DIR)/Audio.cpp \
    $(SOURCE_DIR)/COM8251A.cpp \
    $(SOURCE_DIR)/BasicProgramLoader.cpp \
    $(SOURCE_DIR)/Cartridge.cpp \
    $(SOURCE_DIR)/MachineIOPorts.cpp \
    $(SOURCE_DIR)/cpu/clk/CLKZ80Processor.cpp \
    $(SOURCE_DIR)/cpu/clk/CLKZ80StateAccess.cpp \
    $(SOURCE_DIR)/cpu/clk/GearSF7000ClockDomains.cpp \
    $(SOURCE_DIR)/cpu/clk/GearSF7000BusHandler.cpp \
    $(SOURCE_DIR)/DebugEvents.cpp \
    $(SOURCE_DIR)/DiagnosticLog.cpp \
    $(SOURCE_DIR)/GearSF7000Core.cpp \
    $(SOURCE_DIR)/Input.cpp \
    $(SOURCE_DIR)/Led.cpp \
    $(SOURCE_DIR)/Memory.cpp \
    $(SOURCE_DIR)/SystemClock.cpp \
    $(SOURCE_DIR)/TMS9918CpuAccessTiming.cpp \
    $(SOURCE_DIR)/TMS9918VramSlotSchedule.cpp \
    $(SOURCE_DIR)/TMS9918VramSequencer.cpp \
    $(SOURCE_DIR)/Video.cpp \
    $(SOURCE_DIR)/WAVUtils.cpp \
    $(SOURCE_DIR)/sk1100.cpp \
    $(SOURCE_DIR)/audio/Blip_Buffer.cpp \
    $(SOURCE_DIR)/audio/Effects_Buffer.cpp \
    $(SOURCE_DIR)/audio/Sms_Apu.cpp \
    $(SOURCE_DIR)/audio/Multi_Buffer.cpp \
    $(PROJECT_ROOT)/third_party/clk/Processors/Z80/Implementation/PartialMachineCycle.cpp \
    $(PROJECT_ROOT)/third_party/clk/Processors/Z80/Implementation/Z80Base.cpp \
    $(PROJECT_ROOT)/third_party/clk/Processors/Z80/Implementation/Z80Storage.cpp

CORE_DEFINES := -DGEARSF7000_PRODUCT_SC3000=1 \
    -DGEARSF7000_ENABLE_DEBUG_TOOLS=0 \
    -DGEARSF7000_ENABLE_RECORDER=0 \
    -DGEARSF7000_ENABLE_MCP=0 \
    -DGEARSF7000_ENABLE_AY=0 \
    -DGEARSF7000_ENABLE_SF7000=0 \
    -DGEARSF7000_ENABLE_SR1000=0 \
    -DGEARSF7000_ENABLE_SP400=0 \
    -DGEARSF7000_ENABLE_SDL_INPUT=0 \
    -DGEARSF7000_DISABLE_DISASSEMBLER \
    -DGC_AUDIO_SAMPLE_RATE=48000

ARCH ?= $(shell uname -m)
CFLAGS := -O3 -DNDEBUG -arch $(ARCH) -mmacosx-version-min=11.0 -fPIC \
    $(CORE_DEFINES) -I$(SOURCE_DIR) -I$(PROJECT_ROOT)/third_party/clk
CXXFLAGS := $(CFLAGS) -std=c++20
LDFLAGS := -bundle -arch $(ARCH) -mmacosx-version-min=11.0 \
    -F$(FRAMEWORKS) -framework OpenEmuBase -framework Cocoa

CORE_OBJECTS := $(patsubst $(PROJECT_ROOT)/%,build/%.o,$(SOURCES_CXX)) \
    $(patsubst $(PROJECT_ROOT)/%,build/%.o,$(SOURCES_C))
ADAPTER_OBJECT := build/platforms/openemu/GearSC3000GameCore.mm.o

all: $(BUNDLE)

check:
	@test -f "$(SDK_ROOT)/OpenEmuBase/OEGameCore.h" || \
	    (echo "OpenEmu SDK not found: $(SDK_ROOT)"; exit 1)
	@test -f "$(FRAMEWORKS)/OpenEmuBase.framework/OpenEmuBase" || \
	    (echo "OpenEmu not found: $(OPENEMU_APP)"; exit 1)

$(BUNDLE): check $(EXECUTABLE) Info.plist
	@mkdir -p "$@/Contents"
	cp Info.plist "$@/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(VERSION)" "$@/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(VERSION)" "$@/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :SUFeedURL $(APPCAST_URL)" "$@/Contents/Info.plist"
	codesign $(CODE_SIGN_OPTIONS) "$@"
	@echo "Built $@"

package: $(BUNDLE)
	@rm -f "$(ARCHIVE)" "$(APPCAST)" "$(OPENEMU_SILICON_APPCAST)" "$(CHECKSUM)"
	ditto -c -k --sequesterRsrc --keepParent "$(BUNDLE)" "$(ARCHIVE)"
	cd "$(BUILD_DIR)" && shasum -a 256 "$(notdir $(ARCHIVE))" > "$(notdir $(CHECKSUM))"
	./generate_appcast.sh "$(VERSION)" "$(RELEASE_TAG)" "$(ARCHIVE)" "$(DOWNLOAD_URL)" "$(APPCAST)"
	cp "$(APPCAST)" "$(OPENEMU_SILICON_APPCAST)"
	@echo "Packaged $(ARCHIVE)"
	@echo "Appcast  $(APPCAST)"
	@echo "Silicon  $(OPENEMU_SILICON_APPCAST)"
	@echo "SHA-256 $(CHECKSUM)"

$(EXECUTABLE): $(CORE_OBJECTS) $(ADAPTER_OBJECT)
	@mkdir -p "$(@D)"
	$(CXX) $(LDFLAGS) -o "$@" $^

build/%.cpp.o: $(PROJECT_ROOT)/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -c -o "$@" "$<"

build/%.c.o: $(PROJECT_ROOT)/%.c
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -std=gnu99 -c -o "$@" "$<"

$(ADAPTER_OBJECT): GearSC3000GameCore.mm GearSC3000GameCore.h OESG1000SystemResponderClient.h
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -fobjc-arc -I$(SDK_ROOT) -c -o "$@" "$<"

install: $(BUNDLE)
	open "$(abspath $(BUNDLE))"

clean:
	rm -rf "$(BUILD_DIR)"

.PHONY: all check clean install package
