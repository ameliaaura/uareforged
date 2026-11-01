// Reforge ATMOSPHERE night windows on HD (V2) building trims: the lit-window mask of one trim albedo.
// GL-free (atmos_gl.cpp reads the albedo back and uploads the result; a test harness can include it too).
//
// The HD trims paint whole windows (~20 px panes at 256 px, frames, mullions, arches), so the 1998 facade rule
// (a 3x3 dark block darker than its 9x9 surround) only finds pane corners. Here a window = a connected dark region
// (pane pixels clearly darker than the local wall, mullions closed by a 1 px dilation) whose box is window-sized
// and mostly filled, inside a lit wall. Long dark bands (railings, rubble rows, roof edges) and the black atlas
// background fail the size / wall tests and stay dark.
//
// Each window gets one hash h and is lit in at most one of four sets (RGBA channels), so a draw can pick a
// different set per trim tile (atmos V2 shader: floor(uv) hash) and the pattern does not repeat every tile:
//   R = warm set A (h < 0.145), G = warm set B (0.5 <= h < 0.645), B = cold set C (0.145 <= h < 0.167),
//   A = cold set D (0.645 <= h < 0.667).
// A tile shows one warm + one cold set = ~1 in 6 windows lit, 1 in 8 of those cold: the approved density of the
// 1998 path (fewer windows: ~17 % of the cells, cold for hs > 0.975).
#pragma once
#include <stdint.h>
#include <vector>

namespace AtmosWin
{
struct Stats
{
    int windows = 0;  // accepted window components
    int rejected = 0; // dark components failing the size / fill / shape tests
};

// One mip of a DDS file holding BC1 (DX10 DXGI 71/72 or FourCC DXT1) decoded to RGBA8 (stored encoding): the
// trims' .dds (2048 px BC1 sRGB, full chain). Picks the first level with w <= maxW. Reading the mip back from GL
// instead cost ~200 ms per BC1 trim at level load (driver-side decode). false = not BC1 / short / no such level.
bool DecodeDdsBc1Mip(const std::vector<uint8_t> &f, int maxW, std::vector<uint8_t> &rgba, int &w, int &h);

// rgba: w x h RGBA8 (stored encoding = display), row 0 = first row in GL memory. out: w x h RGBA8 mask.
Stats BuildTrimMask(const uint8_t *rgba, int w, int h, std::vector<uint8_t> &out);
} // namespace AtmosWin
