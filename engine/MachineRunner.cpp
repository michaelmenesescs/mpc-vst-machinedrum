#include "MachineRunner.h"

#include <cstring>
#include <stdexcept>

#include "mc68k/mc68k.h"
#include "mc68k/cpuState.h"
#include "Musashi/m68k.h"

namespace md::engine
{
	namespace
	{
		constexpr uint32_t kScratchBase = 0x01000000, kScratchSize = 0x10000;	// ColdFire internal SRAM range
		constexpr uint32_t kParams = kScratchBase + 0x100, kOut = kScratchBase + 0x200;
		constexpr uint32_t kStackTop = kScratchBase + kScratchSize - 0x10;
		constexpr uint32_t kReturnSentinel = 0x00000100;	// unmapped, never executed: we stop when PC gets here
		constexpr uint64_t kMaxInstructions = 100'000;
		constexpr size_t kRecordSize = 86;
	}

	// The OS image is read-only at $200000; a small scratch RAM at $01000000 holds the stack, params and output.
	class MachineCpu final : public mc68k::Mc68k
	{
	public:
		MachineCpu(const std::vector<uint8_t>& _os) : Mc68k(M68K_CPU_TYPE_MCF5206E), m_os(_os), m_ram(kScratchSize, 0) {}

		uint8_t read8(const uint32_t _addr) override
		{
			if(const auto* p = ptr(_addr, 1)) return *p;
			++m_badAccesses;
			return 0;
		}
		uint16_t read16(const uint32_t _addr) override
		{
			if(const auto* p = ptr(_addr, 2)) return static_cast<uint16_t>(p[0] << 8 | p[1]);
			++m_badAccesses;
			return 0;
		}
		uint16_t readImm16(const uint32_t _addr) override { return read16(_addr); }
		void write8(const uint32_t _addr, const uint8_t _val) override
		{
			if(auto* p = ramPtr(_addr, 1)) *p = _val; else ++m_badAccesses;
		}
		void write16(const uint32_t _addr, const uint16_t _val) override
		{
			if(auto* p = ramPtr(_addr, 2)) { p[0] = static_cast<uint8_t>(_val >> 8); p[1] = static_cast<uint8_t>(_val); }
			else ++m_badAccesses;
		}
		uint32_t exec() override { return execInstruction(); }	// CPU only: no on-chip peripherals

		void poke32(const uint32_t _a, const uint32_t _v) { write16(_a, static_cast<uint16_t>(_v >> 16)); write16(_a + 2, static_cast<uint16_t>(_v)); }
		uint32_t peek32(const uint32_t _a) { return static_cast<uint32_t>(read16(_a)) << 16 | read16(_a + 2); }
		void setAReg(const int _i, const uint32_t _v) { m68k_set_reg(getCpuState(), static_cast<m68k_register_t>(M68K_REG_A0 + _i), _v); }
		void setDReg(const int _i, const uint32_t _v) { m68k_set_reg(getCpuState(), static_cast<m68k_register_t>(M68K_REG_D0 + _i), _v); }
		void setSR(const uint32_t _v) { m68k_set_reg(getCpuState(), M68K_REG_SR, _v); }

		uint32_t m_badAccesses = 0;

	private:
		const uint8_t* ptr(const uint32_t _addr, const uint32_t _n) const
		{
			if(_addr >= MachineRunner::kOsBase && _addr + _n <= MachineRunner::kOsBase + m_os.size()) return &m_os[_addr - MachineRunner::kOsBase];
			if(_addr >= kScratchBase && _addr + _n <= kScratchBase + kScratchSize) return &m_ram[_addr - kScratchBase];
			return nullptr;
		}
		uint8_t* ramPtr(const uint32_t _addr, const uint32_t _n)
		{
			if(_addr >= kScratchBase && _addr + _n <= kScratchBase + kScratchSize) return &m_ram[_addr - kScratchBase];
			return nullptr;
		}
		const std::vector<uint8_t>& m_os;
		std::vector<uint8_t> m_ram;
	};

}

#define MC68K_CLASS md::engine::MachineCpu
#include "mc68k/musashiEntry.h"

namespace md::engine
{
	MachineRunner::MachineRunner(std::vector<uint8_t> _osImage) : m_os(std::move(_osImage))
	{
		m_index.fill(-1);
		m_cpu = std::make_unique<MachineCpu>(m_os);
		m_cpu->setSR(0x2700);	// supervisor, interrupts masked
		parseMachineTable();
	}

	MachineRunner::~MachineRunner() = default;

	void MachineRunner::parseMachineTable()
	{
		// The descriptor table (86-byte records) is found by its shape around the TRX-B2 record's name.
		auto valid = [&](const size_t _o)
		{
			if(_o + kRecordSize > m_os.size()) return false;
			const uint32_t fn = static_cast<uint32_t>(m_os[_o]) << 24 | m_os[_o + 1] << 16 | m_os[_o + 2] << 8 | m_os[_o + 3];
			if(fn < kOsBase || fn >= kOsBase + m_os.size()) return false;
			for(size_t k = 5; k < 10; ++k)
				if(m_os[_o + k] != 0 && (m_os[_o + k] < 32 || m_os[_o + k] >= 127)) return false;
			return true;
		};
		static const char kAnchor[] = "TRXB2PTCH";
		const auto* hit = static_cast<const uint8_t*>(memmem(m_os.data(), m_os.size(), kAnchor, sizeof(kAnchor) - 1));
		if(!hit || hit - m_os.data() < 5) throw std::runtime_error("machine table not found (not an MD OS 1.63 image?)");
		size_t start = static_cast<size_t>(hit - m_os.data()) - 5;
		while(start >= kRecordSize && valid(start - kRecordSize)) start -= kRecordSize;
		for(size_t o = start; valid(o); o += kRecordSize)
		{
			MachineInfo m;
			m.function = static_cast<uint32_t>(m_os[o]) << 24 | m_os[o + 1] << 16 | m_os[o + 2] << 8 | m_os[o + 3];
			m.id = m_os[o + 4];
			for(size_t k = 5; k < 10 && m_os[o + k]; ++k) m.name += static_cast<char>(m_os[o + k]);
			for(size_t p = 0; p < 8; ++p)
			{
				for(size_t k = 0; k < 4 && m_os[o + 10 + 4 * p + k]; ++k) m.params[p] += static_cast<char>(m_os[o + 10 + 4 * p + k]);
				m.defaults[p] = m_os[o + 42 + p];
			}
			m_index[m.id] = static_cast<int>(m_machines.size());
			m_machines.push_back(std::move(m));
		}
	}

	const MachineInfo* MachineRunner::machine(const uint8_t _id) const
	{
		return m_index[_id] < 0 ? nullptr : &m_machines[static_cast<size_t>(m_index[_id])];
	}

	int MachineRunner::compute(const uint8_t _machineId, const uint16_t* _params, uint32_t* _out, const int _outCapacity)
	{
		m_fault.clear();
		const auto* m = machine(_machineId);
		if(!m) { m_fault = "unknown machine"; return -1; }

		auto& cpu = *m_cpu;
		for(uint32_t k = 0; k < 24; ++k)
			cpu.write16(kParams + 2 * k, k < 8 ? _params[k] : 0);
		for(uint32_t k = 0; k < 32; ++k)
			cpu.poke32(kOut + 4 * k, 0);

		// C calling convention: fn(out, params), return address on top of the stack
		uint32_t sp = kStackTop;
		sp -= 4; cpu.poke32(sp, kParams);
		sp -= 4; cpu.poke32(sp, kOut);
		sp -= 4; cpu.poke32(sp, kReturnSentinel);
		cpu.setAReg(7, sp);
		cpu.setPC(m->function);
		cpu.m_badAccesses = 0;

		uint64_t n = 0;
		while(cpu.getPC() != kReturnSentinel)
		{
			cpu.exec();
			if(++n > kMaxInstructions) { m_fault = "instruction budget exceeded"; return -1; }
		}
		m_lastInstructions = n;
		if(cpu.m_badAccesses) { m_fault = "access outside the OS image/scratch RAM"; return -1; }

		const int count = static_cast<int>(cpu.getDReg(0));
		for(int k = 0; k < _outCapacity; ++k)
			_out[k] = cpu.peek32(kOut + 4 * static_cast<uint32_t>(k));
		return count;
	}
}
