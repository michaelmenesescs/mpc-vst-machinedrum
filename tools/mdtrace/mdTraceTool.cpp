// mdTraceTool: local investigation tool (mpc-vst-machinedrum), not part of gearmulator-md-mm.
// Boots the MD device from a user-supplied ROM, plays one note on one track, and (with
// MD_TRACE=1) captures every UC<->DSP HI08 word via the tracing added to mddsp.cpp locally.
#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "synthLib/plugin.h"
#include "baseLib/filesystem.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::cerr << "usage: mdTraceTool ROM.bin [note] [frames]\n";
		return 2;
	}

	std::vector<uint8_t> firmware;
	if(!baseLib::filesystem::readFile(firmware, argv[1]))
	{
		std::cerr << "cannot read " << argv[1] << '\n';
		return 1;
	}

	const auto model = md::MachineModel::Machinedrum;
	if(!md::RomLoader::isRomForModel(firmware, model))
	{
		std::cerr << "not a recognised MD 1.63 image\n";
		return 1;
	}

	const uint8_t note = argc > 2 ? static_cast<uint8_t>(std::atoi(argv[2])) : 36;
	const uint32_t frames = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 4096;

	synthLib::DeviceCreateParams params;
	params.romData = std::move(firmware);
	params.romName = argv[1];
	params.customData = md::deviceCustomData(model);
	auto device = std::make_unique<md::Device>(params);
	if(!device->isValid())
	{
		std::cerr << "device did not become valid\n";
		return 1;
	}

	synthLib::Plugin plugin(device.get(), [](synthLib::Device*) {});
	plugin.reserveMidiEventCapacity();
	plugin.setHostSamplerate(44100.0f, 44100.0f);
	plugin.setBlockSize(512);

	constexpr size_t capacity = 512;
	std::array<float, capacity> left{}, right{};
	std::array<std::array<float, capacity>, 6> output{};
	const synthLib::TAudioInputs inputs{ left.data(), right.data(), nullptr, nullptr };
	synthLib::TAudioOutputs outputs{};
	for(size_t c = 0; c < output.size(); ++c)
		outputs[c] = output[c].data();

	auto process = [&](uint32_t remaining, const char* label)
	{
		std::cerr << "-- " << label << " --\n";
		while(remaining > 0)
		{
			const auto chunk = std::min<uint32_t>(static_cast<uint32_t>(capacity), remaining);
			synthLib::TAudioInputs in{ inputs[0], inputs[1], inputs[2], inputs[3] };
			synthLib::TAudioOutputs out;
			for(size_t c = 0; c < output.size(); ++c) out[c] = outputs[c];
			plugin.process(in, out, chunk, 120.0, 0.0, true);
			remaining -= chunk;
		}
	};

	// Let the boot/init sequence run to completion before the note.
	process(frames, "boot+idle");

	// Optional script: argv[4..] = "on:NOTE:VEL:FRAMES", "off:NOTE:FRAMES", "cc:CH:CC:VAL:FRAMES",
	// "wait:FRAMES". Each event is sent, then FRAMES frames are processed. Channel is 0-based.
	for(int a = 4; a < argc; ++a)
	{
		const std::string s = argv[a];
		std::vector<int> v;
		std::string kind = s.substr(0, s.find(':'));
		for(size_t p = s.find(':'); p != std::string::npos; p = s.find(':', p + 1))
			v.push_back(std::atoi(s.c_str() + p + 1));
		uint32_t wait = v.empty() ? 0u : static_cast<uint32_t>(v.back());
		if(kind == "on" && v.size() >= 3)
			plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEON, static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), 0});
		else if(kind == "off" && v.size() >= 2)
			plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEOFF, static_cast<uint8_t>(v[0]), 0, 0});
		else if(kind == "cc" && v.size() >= 4)
			plugin.addMidiEvent({synthLib::MidiEventSource::Host, static_cast<uint8_t>(synthLib::M_CONTROLCHANGE | (v[0] & 15)), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2]), 0});
		process(wait, s.c_str());
	}

	if(note != 0 && argc <= 4)
	{
		plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEON, static_cast<uint8_t>(note), 100, 0});
		process(frames, "note-on");

		plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEOFF, static_cast<uint8_t>(note), 0, 0});
		process(frames, "note-off");
	}

	std::cerr << "mdTraceTool: done\n";
	return 0;
}
