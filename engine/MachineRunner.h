// Runs the Machinedrum OS's own per-machine coefficient functions (compiled C in the ColdFire OS image,
// section 0 of the user's .syx) in a 68k emulator. Each is count = fn(uint32_t* out, const uint16_t* params):
// the 8 SYN parameters in, out[1..12] = the voice DSP's slot words. The machine descriptor table gives each
// machine's function. See docs/PROTOCOL.md, "Host model: the ColdFire side".
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace md::engine
{
	class MachineCpu;

	struct MachineInfo
	{
		uint8_t id = 0;
		std::string name;
		std::array<std::string, 8> params;
		std::array<uint8_t, 8> defaults{};
		uint32_t function = 0;
	};

	class MachineRunner
	{
	public:
		static constexpr uint32_t kOsBase = 0x200000;

		explicit MachineRunner(std::vector<uint8_t> _osImage);	// decompressed section 0
		~MachineRunner();

		const std::vector<MachineInfo>& machines() const { return m_machines; }
		const MachineInfo* machine(uint8_t _id) const;

		// Call a machine's coefficient function. params: at least the 8 SYN values as 16-bit (value << 7, up to
		// $3fff); out receives the words the OS writes (out[0] is left 0: it is the caller's trigger word).
		// Returns the word count the function reports, or -1 on a fault.
		int compute(uint8_t _machineId, const uint16_t* _params, uint32_t* _out, int _outCapacity);

		uint64_t lastInstructions() const { return m_lastInstructions; }
		const std::string& faultReason() const { return m_fault; }

	private:
		void parseMachineTable();

		std::vector<uint8_t> m_os;
		std::unique_ptr<MachineCpu> m_cpu;
		std::vector<MachineInfo> m_machines;
		std::array<int, 256> m_index{};
		uint64_t m_lastInstructions = 0;
		std::string m_fault;
	};
}
