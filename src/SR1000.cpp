#include "SR1000.h"
#include "Memory.h"
#include "Audio.h"
#include "SegaBasicBitTape.h"
#include <cctype>
#include <algorithm>
#include <utility>

SR1000::SR1000(Audio* audio, Memory* memory)
    : SR1000(audio, memory, SR1000DebugConfiguration{}) {}

SR1000::SR1000(Audio* audio, Memory* memory, SR1000DebugConfiguration debug)
    : TapeRecorder(TapeMotor::Policy::AlwaysOn), m_debug(std::move(debug))
{
    m_monitor = [audio](float sample, bool synthesized, float frequency) {
        if (audio) audio->SR1000SpeakerWrite(sample, synthesized, frequency);
    };
    m_events = [memory](unsigned event, std::uint8_t value) {
        if (memory) memory->CheckTapeEvent(event, value);
    };
}

DebugDeviceDefinition SR1000::DescribeDebugDevice() const
{
	DebugDeviceDefinition device;
	device.key = m_debug.key;
	device.parentKey = m_debug.parentKey;
	device.typeKey = "sega.sr1000";
	device.name = m_debug.name.empty() ? "SR-1000 Tape" : m_debug.name;
	device.traits = DebugDeviceTrait_External;

	DebugSpaceDefinition state;
	state.id = DebugCommonSpace::State;
	state.key = "state";
	state.name = "Tape State";
	state.targetMax = TapeEventMotor;
	state.supportedAccesses = DebugEventAccess_Event;
	state.afterValueAccesses = DebugEventAccess_Event;
	state.capabilities = DebugDescriptorCapability_EventsPublished;
	state.targets.push_back({TapeEventLoaded, "Loaded", 1});
	state.targets.push_back({TapeEventPlaying, "Playing", 1});
	state.targets.push_back({TapeEventRewind, "Rewind", 1});
	state.targets.push_back({TapeEventTargetSpeed, "Target Speed", 8});
	state.targets.push_back({TapeEventMotor, "Motor", 1});
	device.spaces.push_back(std::move(state));

	DebugRulePresetDefinition preset;
	preset.key = "state";
	preset.name = "Tape";
	preset.spaceKey = "state";
	preset.targetEnd = TapeEventMotor;
	preset.accessMask = DebugEventAccess_Event;
	device.presets.push_back(std::move(preset));
	return device;
}

static std::string getExtension(const std::string& filename) {
	size_t pos = filename.rfind('.');
	if (pos != std::string::npos) {
		std::string extension = filename.substr(pos);
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return extension;
	}
	return ""; // Nessuna estensione trovata
}


bool SR1000::LoadTape(std::string filename)
{
	if (filename == "") {
		Eject();
		return true;
	}

	std::string extension = getExtension(filename);
	if (extension != ".bit" && extension != ".bas" && extension != ".basic")
		return TapeRecorder::LoadTape(filename);

	bool loaded = false;
	Tape* loadedTape = nullptr;

	if (extension == ".bit")
	{
		SegaBasicBitTape* bitTape = new SegaBasicBitTape();
		if (bitTape)
		{
			bitTape->Init(MasterClock, this->CassetteMotor);
			loaded = bitTape->LoadTape(filename);
			loadedTape = bitTape;
		}
	}
	else if (extension == ".bas" || extension == ".basic")
	{
		SegaBasicBitTape* bitTape = new SegaBasicBitTape();
		if (bitTape)
		{
			bitTape->Init(MasterClock, this->CassetteMotor);
			loaded = bitTape->LoadBas(filename);
			loadedTape = bitTape;
		}
	}

	if (!loaded)
	{
		delete loadedTape;
		return false;
	}

	MountTape(loadedTape);
	return true;
}
