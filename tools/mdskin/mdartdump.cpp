// mdartdump: the LCD artwork of the user's own Machinedrum OS file (the six bitmap fonts, the dial ring, dot and group tie) as the
// JSON tools/mdskin/mk_skin.py draws the plugin's skin from. The Machinedrum and Monomachine OS share the Elektron LCD font family,
// so the skin needs no second OS file. The two-state toggle icon is drawn here, not read from the OS. Addresses are those of
// Machinedrum UW OS 1.63 (the only OS this port supports). The output is Elektron's artwork: per-user build data, never committed.
//   mdartdump <MD OS .syx> <out art.json>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Firmware.h"

namespace
{
constexpr uint32_t kBase = 0x200000;   // address of byte 0 of the main-processor section
std::vector<uint8_t> g_d;
FILE* g_f;

uint32_t u32(uint32_t a)
{
	if(a < kBase || size_t(a - kBase) + 4 > g_d.size()) throw std::runtime_error("address outside the OS image");
	const uint8_t* p = &g_d[a - kBase];
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
const uint8_t* at(uint32_t a, size_t n)
{
	if(a < kBase || size_t(a - kBase) + n > g_d.size()) throw std::runtime_error("address outside the OS image");
	return &g_d[a - kBase];
}

struct Bmp { int w, h; std::vector<uint64_t> rows; };

void put(const Bmp& b)
{
	std::fprintf(g_f, "{\"w\":%d,\"h\":%d,\"rows\":[", b.w, b.h);
	for(int r = 0; r < b.h; ++r) std::fprintf(g_f, "%s\"%016llx\"", r ? "," : "", (unsigned long long)b.rows[size_t(r)]);
	std::fputs("]}", g_f);
}

// icon descriptor: w, h, n, pixel columns pointer, mask pointer; one u32 column per x, top row in the bit (32 - h)
Bmp icon(uint32_t a, int wantW, int wantH)
{
	const uint32_t w = u32(a), h = u32(a + 4), n = u32(a + 8), px = u32(a + 12), mask = u32(a + 16);
	if(int(w) != wantW || int(h) != wantH || n < 1 || n > 4 || mask != px + 4 * w) throw std::runtime_error("not the expected icon");
	Bmp b{int(w), int(h), std::vector<uint64_t>(h, 0)};
	for(uint32_t c = 0; c < w; ++c)
	{
		const uint32_t col = u32(px + 4 * c);
		for(uint32_t r = 0; r < h; ++r)
			if((col >> (32 - h + r)) & 1) b.rows[r] |= uint64_t(1) << (63 - c);
	}
	return b;
}

// font descriptor: advance, height, widths[256], offsets[128 x i16], one byte per glyph column (top row in bit 8-h)
void font(const char* name, uint32_t a, int wantH, bool last)
{
	const uint32_t adv = u32(a), h = u32(a + 4), widthsAt = u32(a + 8), offsetsAt = u32(a + 12), colsAt = u32(a + 16);
	if(int(h) != wantH || adv < 1 || adv > 16) throw std::runtime_error(std::string("not the expected font: ") + name);
	const uint8_t* widths = at(widthsAt, 256);
	const uint8_t* offsets = at(offsetsAt, 512);
	std::fprintf(g_f, "\"%s\":{\"h\":%u,\"adv\":%u,\"glyphs\":{", name, h, adv);
	bool first = true;
	for(int c = 0; c < 128; ++c)
	{
		const int off = int16_t((offsets[2 * c] << 8) | offsets[2 * c + 1]);
		if(off < 0) continue;
		const uint32_t w = widths[c] ? widths[c] : adv;
		if(w > 16) throw std::runtime_error("font glyph too wide");
		const uint8_t* cols = at(colsAt + uint32_t(off), w);
		Bmp b{int(w), int(h), std::vector<uint64_t>(h, 0)};
		for(uint32_t x = 0; x < w; ++x)
			for(uint32_t r = 0; r < h; ++r)
				if((cols[x] >> (8 - h + r)) & 1) b.rows[r] |= uint64_t(1) << (63 - x);
		std::fprintf(g_f, "%s\"%d\":", first ? "" : ",", c);
		first = false;
		put(b);
	}
	std::fprintf(g_f, "}}%s", last ? "" : ",");
}

// The OFF/ON icon of the randomise cells: a ring with a pointer (up-left = off, up-right = on), drawn for this project.
Bmp toggle(bool on)
{
	Bmp b{17, 13, std::vector<uint64_t>(13, 0)};
	auto set = [&](int x, int y) { b.rows[size_t(y)] |= uint64_t(1) << (63 - x); };
	const char* ring[11] = {"...#####...", "..#.....#..", ".#.......#.", "#.........#", "#.........#", "#.........#",
	                        "#.........#", "#.........#", ".#.......#.", "..#.....#..", "...#####..."};
	for(int y = 0; y < 11; ++y)
		for(int x = 0; x < 11; ++x)
			if(ring[y][x] == '#') set(3 + x, 1 + y);
	for(int i = 0; i < 4; ++i) set(on ? 8 + 1 + i : 8 - 1 - i, 6 - i);   // pointer from the centre
	return b;
}
} // namespace

int main(int argc, char** argv)
{
	if(argc != 3) { std::fprintf(stderr, "usage: mdartdump <MD OS .syx> <out art.json>\n"); return 2; }
	try
	{
		const auto ct = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		if(ct.sections.empty()) throw std::runtime_error("no main-processor section");
		g_d = ct.sections[0].data;
		g_f = std::fopen(argv[2], "w");
		if(!g_f) throw std::runtime_error("cannot write the output");
		std::fputs("{\"fonts\":{", g_f);
		font("tiny3x5", 0x259954, 5, false);
		font("small4x5", 0x259968, 5, false);
		font("square5x5", 0x25997c, 5, false);
		font("bold8", 0x259990, 8, false);
		font("digitsTop", 0x2599a4, 5, false);
		font("digitsBottom", 0x2599b8, 5, true);
		std::fputs("},\"bitmaps\":{\"dialRing\":", g_f);
		put(icon(0x2599e0, 11, 13));
		std::fputs(",\"ringPlain\":", g_f);
		put(icon(0x2599cc, 11, 11));
		std::fputs(",\"groupTie\":", g_f);
		put(icon(0x259af8, 7, 3));
		std::fputs(",\"dialDot\":[", g_f);
		for(int i = 0; i < 128; ++i) { if(i) std::fputc(',', g_f); put(icon(0x259b0c + 20 * uint32_t(i), 7, 7)); }
		std::fputs("],\"toggle\":[", g_f);
		put(toggle(false));
		std::fputc(',', g_f);
		put(toggle(true));
		std::fputs("]}}\n", g_f);
		std::fclose(g_f);
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "mdartdump: %s (this needs Machinedrum UW OS 1.63)\n", e.what());
		return 1;
	}
	return 0;
}
