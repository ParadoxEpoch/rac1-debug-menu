// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the OpenRAC contributors
//
// Opens the retail I-5 debug menu (game mode -1). The menu's update and draw are
// still in every level; retail never stores -1. R3 during play latches an open
// and is taken out of the pad's newly-pressed bits, so the map (which shares
// that edge with Select) does not open. on_frame then saves the current mode
// into g_debug_menu.buffer_mode and sets the mode word to -1, after this frame's
// update so the same R3 edge cannot close it. The menu's own exit (R3 or Select)
// restores buffer_mode. Invinc is wired to Hero_TakeDamage because nothing else
// in retail reads that flag.
//
// The menu clears the frame with fs_aa_clear_packet: 16 untextured strips as tall
// as the screen. The port leaves those out (it clears the frame itself, and a
// full-height fill would cover the world). With no world drawn, that leaves the
// last picture up and each new line of text on top of the old one. While the menu
// is open the same 16 strips are retiled into short bands that cover the same
// rectangle, in black, so the port draws them under the text. The original strips
// are put back when the menu closes, so gameplay clears stay omitted.
//
// On PAL the font is uploaded once, at the 50 Hz graphics base. The port then
// runs the game at 60 Hz, and the texture heap starts below that font. Kerwan
// and Eudora (and only those of the levels tried) place one image on the
// font's exact block, so the glyphs sample that image and the menu is black.
// The block stays replaced until the title loads the font again. After each
// level load this mod uploads the font at the base the game is using now.
#include "openrac/mod.hpp"

#include "openrac/games/rac1.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Game pad bit for R3 (active-high in PadState.pressed).
constexpr std::int32_t kButtonR3 = 0x0400;

// Newly-pressed copies (games/rac1/docs/kb/data-structures.md). The map opens on
// pressed & (R3|Select). held is left alone.
constexpr openrac::gaddr kPadPressed = 0x1A4;
constexpr openrac::gaddr kPadRawPressed = 0x1B4;
constexpr openrac::gaddr kPadPressedUnmasked = 0x1C4;

// DebugMenuState: invincibility_enabled at +0x24, buffer_mode (saved exit mode) at +0xA4.
constexpr openrac::gaddr kMenuInvinc = 0x24;
constexpr openrac::gaddr kMenuBufferMode = 0xA4;

// Level-0 addresses of gGameMode and g_debug_menu, the resident pad 0 base, and
// fs_aa_clear_packet (resident, not level data). PAL D_00152140, NTSC D_00152040.
// Ten qwords of header, then 16 pairs of XYZF2. Word 7 is the clear colour
// (SetBackgroundColor). games/rac1/ntsc/include/rnc/rendering/fs_aa_packets.h.
struct DiscAddresses {
    openrac::gaddr game_mode;
    openrac::gaddr debug_menu;
    openrac::gaddr pad0;
    openrac::gaddr clear_packet;
};

constexpr DiscAddresses kPal{0x0015F6A8u, 0x0016C158u, 0x0013CA40u, 0x00152140u};
constexpr DiscAddresses kNtsc{0x0015F5C4u, 0x0016C058u, 0x0013C940u, 0x00152040u};

constexpr int kClearStrips = 16;
constexpr int kClearHeaderWords = 10;
constexpr openrac::gaddr kWord = 8;
// DMA source tag the setup routine writes at the start of the packet.
constexpr std::uint64_t kClearPacketTag = 0x1000000000000001ull;
// RGBAQ black, alpha 0x80 (GS 1.0). Opaque, so the bands replace the old frame.
constexpr std::uint64_t kClearBlack = 0x80000000ull;

std::uint64_t s_saved_strips[kClearStrips * 2] = {};
bool s_clear_patched = false;

openrac::Mod* s_mod = nullptr;
DiscAddresses s_addr = kPal;
bool s_open_latched = false;
bool s_pal = true;

// PAL boot uploads the debug font at D_0015EF88 + 0xC0000 while the machine is
// still in 50 Hz (z base 0x1E0000, so block 0x2A00). LoadPifAsPSMT8H is
// func_001E94E8: the PIF and the TEX0 it writes back are guest addresses.
constexpr openrac::gaddr kPalFontBuffer = 0x001AAF40u;
constexpr openrac::gaddr kPalFontHeader = 0x0015EFC8u;
constexpr openrac::gaddr kPalZBase = 0x0015EF88u;
constexpr int kFontAboveZ = 0xC0000;
constexpr int kFontClut = 0x3FFC00;
constexpr std::uint32_t kPifMagic = 0x50494632u;

using LoadPifFn = void (*)(openrac::gaddr pif, openrac::gaddr tex0_out, int tbp, int cbp);
LoadPifFn s_load_pif = nullptr;
std::vector<std::uint8_t> s_font;
int s_font_level = -2;
bool s_font_logged = false;

const DiscAddresses& addresses_for(std::string_view version) {
    return version == "ntsc" ? kNtsc : kPal;
}

// The loaded level's copy of a level-0 global. Ask each time: a level moves its data.
openrac::gaddr level_data(openrac::gaddr level0) {
    return s_mod->relocate_level(0, level0);
}

std::int32_t read_game_mode() {
    return s_mod->memory().read<std::int32_t>(level_data(s_addr.game_mode));
}

std::uint16_t coord_x(std::uint64_t word) {
    return static_cast<std::uint16_t>(word);
}

std::uint16_t coord_y(std::uint64_t word) {
    return static_cast<std::uint16_t>(word >> 16);
}

std::uint64_t vertex(std::uint16_t x, int y, std::uint64_t high) {
    return (high & 0xFFFFFFFF00000000ull)
           | static_cast<std::uint16_t>(x)
           | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16);
}

// Cover the saved strips' rectangle with 16 short horizontal bands. Each band is
// far shorter than the frame, so the port draws it instead of treating it as a clear.
void write_clear_bands() {
    const auto mem = s_mod->memory();
    const openrac::gaddr verts = s_addr.clear_packet + kClearHeaderWords * kWord;
    std::uint16_t x0 = 0xFFFF;
    std::uint16_t x1 = 0;
    std::uint16_t y0 = 0xFFFF;
    std::uint16_t y1 = 0;
    for (int i = 0; i < kClearStrips * 2; ++i) {
        const std::uint16_t x = coord_x(s_saved_strips[i]);
        const std::uint16_t y = coord_y(s_saved_strips[i]);
        if (x < x0) {
            x0 = x;
        }
        if (x > x1) {
            x1 = x;
        }
        if (y < y0) {
            y0 = y;
        }
        if (y > y1) {
            y1 = y;
        }
    }
    const int span = static_cast<int>(y1) - static_cast<int>(y0);
    if (x1 <= x0 || span <= 0) {
        return;
    }
    for (int i = 0; i < kClearStrips; ++i) {
        const int top = static_cast<int>(y0) + span * i / kClearStrips;
        const int bottom = static_cast<int>(y0) + span * (i + 1) / kClearStrips;
        const std::uint64_t high = s_saved_strips[i * 2];
        mem.write<std::uint64_t>(verts + static_cast<openrac::gaddr>(i * 2) * kWord,
                                 vertex(x0, top, high));
        mem.write<std::uint64_t>(verts + static_cast<openrac::gaddr>(i * 2 + 1) * kWord,
                                 vertex(x1, bottom, high));
    }
    mem.write<std::uint64_t>(s_addr.clear_packet + 7 * kWord, kClearBlack);
}

void sync_menu_clear() {
    const auto mem = s_mod->memory();
    if (mem.read<std::uint64_t>(s_addr.clear_packet) != kClearPacketTag) {
        return;
    }
    // The packet is resident. Restore it even after a level unloads, or the next
    // level would draw these bands over the world.
    const bool menu = s_mod->loaded_level() >= 0 && read_game_mode() == -1;
    const openrac::gaddr verts = s_addr.clear_packet + kClearHeaderWords * kWord;
    if (menu) {
        if (!s_clear_patched) {
            for (int i = 0; i < kClearStrips * 2; ++i) {
                s_saved_strips[i] =
                    mem.read<std::uint64_t>(verts + static_cast<openrac::gaddr>(i) * kWord);
            }
            s_clear_patched = true;
        }
        write_clear_bands();
        return;
    }
    if (!s_clear_patched) {
        return;
    }
    for (int i = 0; i < kClearStrips * 2; ++i) {
        mem.write<std::uint64_t>(verts + static_cast<openrac::gaddr>(i) * kWord, s_saved_strips[i]);
    }
    s_clear_patched = false;
}

// The font file is still in the boot buffer until a level is copied over it.
// PIF2, 128x128, PSM 0: 32-byte header, a 1024-byte clut, then the indices.
void snapshot_debug_font() {
    if (!s_font.empty()) {
        return;
    }
    const auto mem = s_mod->memory();
    if (mem.read<std::uint32_t>(kPalFontBuffer) != kPifMagic) {
        return;
    }
    const std::int32_t w = mem.read<std::int32_t>(kPalFontBuffer + 8);
    const std::int32_t h = mem.read<std::int32_t>(kPalFontBuffer + 12);
    const std::int32_t psm = mem.read<std::int32_t>(kPalFontBuffer + 20);
    if (w != 128 || h != 128 || psm != 0) {
        return;
    }
    const std::size_t bytes = 32u + 1024u + static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    s_font.resize(bytes);
    for (std::size_t i = 0; i < bytes; i += 8) {
        const std::uint64_t word = mem.read<std::uint64_t>(kPalFontBuffer + static_cast<openrac::gaddr>(i));
        std::memcpy(s_font.data() + i, &word, sizeof(word));
    }
}

// Upload the saved font at the z base the game is using now, and point the
// debug text at it. A level's own copy of the header word is the boot
// placeholder, and Kerwan/Eudora replace the old block, so do this once the
// level's program and textures are in.
void restore_debug_font() {
    if (!s_pal) {
        return;
    }
    snapshot_debug_font();
    const int level = s_mod->loaded_level();
    if (level < 0 || level > 18) {
        s_font_level = -2;
        return;
    }
    if (s_font.empty() || level == s_font_level) {
        return;
    }
    if (s_load_pif == nullptr) {
        if (!s_font_logged) {
            s_font_logged = true;
            s_mod->warn("func_001E94E8 is not in this build; the debug font cannot be re-uploaded");
        }
        return;
    }
    const auto mem = s_mod->memory();
    const int z = mem.read<std::int32_t>(kPalZBase);
    if (z == 0) {
        return;
    }
    openrac::StackFrame file(*s_mod, static_cast<std::uint32_t>(s_font.size()));
    openrac::StackFrame tex(*s_mod, 32);
    if (file.address() == 0 || tex.address() == 0) {
        return;
    }
    for (std::size_t i = 0; i < s_font.size(); i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, s_font.data() + i, sizeof(word));
        mem.write<std::uint64_t>(file.address() + static_cast<openrac::gaddr>(i), word);
    }
    s_load_pif(file.address(), tex.address(), z + kFontAboveZ, kFontClut);
    mem.write<std::uint64_t>(kPalFontHeader, mem.read<std::uint64_t>(tex.address()));
    s_font_level = level;
    if (!s_font_logged) {
        s_font_logged = true;
        s_mod->info("debug font re-uploaded for the graphics base this level uses");
    }
}

// Drop R3 from this frame's new presses so handle_pause_input does not open the map.
void clear_r3_press(std::uint32_t pad) {
    const auto mem = s_mod->memory();
    const openrac::gaddr base = pad;
    for (const openrac::gaddr off : {kPadPressed, kPadRawPressed, kPadPressedUnmasked}) {
        const std::int32_t bits = mem.read<std::int32_t>(base + off);
        mem.write<std::int32_t>(base + off, bits & ~kButtonR3);
    }
}

void load(openrac::Mod& mod) {
    if (mod.game() != "rac1") {
        throw openrac::Error("debug-menu is for Ratchet & Clank (rac1)");
    }

    s_mod = &mod;
    s_addr = addresses_for(mod.version());
    s_pal = mod.version() != "ntsc";
    if (s_pal) {
        s_load_pif = mod.function<LoadPifFn>("func_001E94E8");
    }

    // After the report is turned into pad state: R3's edge during play latches an open.
    // The mode word is left alone here so this frame's update_debug_menu never runs.
    mod.hook<openrac::rac1::Pad_ProcessReport>(
        [](auto original, std::uint32_t pad, std::uint32_t report, std::int32_t size) {
            original(pad, report, size);

            if (pad != s_addr.pad0) {
                return;
            }
            if (s_mod->loaded_level() < 0) {
                return;
            }

            const std::int32_t pressed =
                s_mod->memory().read<std::int32_t>(static_cast<openrac::gaddr>(pad) + kPadPressed);
            if ((pressed & kButtonR3) == 0) {
                return;
            }

            // Already in the menu, or in the map: that press is the menu's own exit.
            // Select is left set, so Select still opens the map.
            if (read_game_mode() != 0) {
                return;
            }

            clear_r3_press(pad);
            s_open_latched = true;
        });

    // After the tick: apply a latched open so the next frame enters case -1, then
    // keep the clear packet matched to whether the menu is up. This runs after
    // the port has drawn, so a restore is in place before the next tick builds
    // its packets, and a patch is in place before the menu's first draw.
    mod.on_frame([] {
        restore_debug_font();
        if (s_open_latched) {
            s_open_latched = false;

            if (s_mod->loaded_level() >= 0) {
                const openrac::gaddr mode_addr = level_data(s_addr.game_mode);
                const openrac::gaddr menu_addr = level_data(s_addr.debug_menu);
                const std::int32_t mode = s_mod->memory().read<std::int32_t>(mode_addr);
                if (mode == 0) {
                    // What update_debug_menu restores on R3/Select (pressed & 0x500).
                    s_mod->memory().write<std::int32_t>(menu_addr + kMenuBufferMode, mode);
                    s_mod->memory().write<std::int32_t>(mode_addr, -1);
                }
            }
        }
        sync_menu_clear();
    });

    // The MODE page toggles invincibility_enabled; retail never reads it.
    mod.hook<openrac::rac1::Hero_TakeDamage>([](auto original, std::int32_t amount) {
        if (s_mod->loaded_level() >= 0) {
            const openrac::gaddr invinc =
                level_data(s_addr.debug_menu) + kMenuInvinc;
            if (s_mod->memory().read<std::int32_t>(invinc) != 0) {
                return;
            }
        }
        original(amount);
    });

    mod.info(std::string("I-5 debug menu on ") + std::string(mod.version())
             + ": R3 (V) during play opens it");
}

}  // namespace

OPENRAC_MOD_CPP("debug-menu", "1.0.0", load)
