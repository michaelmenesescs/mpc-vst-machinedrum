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

	if(note != 0)
	{
		plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEON, static_cast<uint8_t>(note), 100, 0});
		process(frames, "note-on");

		plugin.addMidiEvent({synthLib::MidiEventSource::Host, synthLib::M_NOTEOFF, static_cast<uint8_t>(note), 0, 0});
		process(frames, "note-off");
	}

	std::cerr << "mdTraceTool: done\n";
	return 0;
}
