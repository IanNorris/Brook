#pragma once

#include "chrome_shell_model.h"
#include <stddef.h>

namespace brook {

inline constexpr uint64_t ShellServiceSyscall = 0xB24;
inline constexpr uint32_t ShellServiceVersion = 4;
inline constexpr uint32_t ShellCommandLimit = 1536;
inline constexpr uint32_t ShellTextLimit = 4096;
// One 24x24 icon per launcher row, plus room for the material gradient.
inline constexpr uint32_t ShellAssetLimit = 128 * 24 * 24 + 256;
enum class ShellServiceOp : uint64_t { Register, Receive, Present, TrayPublish, TrayNextInput };

struct ShellExchange {
    uint64_t sequence;
    uint32_t kind;
    uint32_t version;
};

struct ShellSnapshot {
    ChromeShellConfig config;
    ChromeShellModel model;
};

struct ShellWireCommand {
    ChromeOp op;
    int32_t x, y, w, h;
    uint32_t color, arg;
    int32_t dx, dy;
    DesktopRect clip;
    uint32_t asset;
};
struct ShellWireList {
    uint32_t count, textBytes, assetWords;
    ShellWireCommand commands[ShellCommandLimit];
    char text[ShellTextLimit];
    uint32_t assets[ShellAssetLimit];
};

inline bool PackShellList(const ChromeDisplayList& list, ShellWireList& wire)
{
    if (list.Overflowed() || list.Count() > ShellCommandLimit) return false;
    wire.count = list.Count();
    wire.textBytes = wire.assetWords = 0;
    for (uint32_t i = 0; i < wire.count; ++i) {
        const auto& c = list.At(i);
        auto& dst = wire.commands[i];
        dst = {c.op, c.x, c.y, c.w, c.h, c.color, c.arg, c.dx, c.dy, c.clip, 0};
        if (c.op == ChromeOp::Text) {
            dst.arg = wire.textBytes;
            const char* str = list.TextAt(c.arg);
            char ch;
            do {
                if (wire.textBytes == ShellTextLimit) return false;
                ch = *str++;
                wire.text[wire.textBytes++] = ch;
            } while (ch);
        }
        if (c.op == ChromeOp::Rows || c.op == ChromeOp::Icon) {
            const uint32_t words = c.op == ChromeOp::Rows ? uint32_t(c.h) : 24 * 24;
            if (!c.data || words > ShellAssetLimit - wire.assetWords) return false;
            dst.asset = wire.assetWords;
            const auto* source = static_cast<const uint32_t*>(c.data);
            for (uint32_t j = 0; j < words; ++j) wire.assets[wire.assetWords++] = source[j];
        }
    }
    return true;
}

inline bool DecodeShellCommand(const ShellWireCommand& c,
                               const char* text, uint32_t textBytes,
                               const uint32_t* assets, uint32_t assetWords,
                               ChromeCmd& out)
{
    auto coordinate = [](int32_t v) { return v >= -8192 && v <= 8192; };
    if (!coordinate(c.x) || !coordinate(c.y) || !coordinate(c.w) || !coordinate(c.h) ||
        !coordinate(c.dx) || !coordinate(c.dy) || !coordinate(c.clip.x) ||
        !coordinate(c.clip.y) || !coordinate(c.clip.w) || !coordinate(c.clip.h)) return false;
    const void* data = nullptr;
    uint32_t words = 0;
    switch (c.op) {
    case ChromeOp::FadeX:
    case ChromeOp::Fill:
        break;
    case ChromeOp::Clip:
        if (c.w < 0 || c.h < 0) return false;
        break;
    case ChromeOp::Rows:
        if (c.h < 0 || c.h > 256) return false;
        words = c.h;
        break;
    case ChromeOp::Icon:
        if (c.w < 1 || c.w > 128 || c.arg > 255) return false;
        words = 24 * 24;
        break;
    case ChromeOp::CornerFill:
    case ChromeOp::MaterialOutline:
    case ChromeOp::MaterialRect:
        if (c.arg > 128) return false;
        break;
    case ChromeOp::Squares:
        if (c.arg > 512) return false;
        break;
    case ChromeOp::Text: {
        if (c.dx < 0 || c.dx > 1 || c.dy < 0 || c.dy >= int32_t(ChromeFont::Count) ||
            c.arg >= textBytes) return false;
        uint32_t end = c.arg;
        while (end < textBytes && text[end]) ++end;
        if (end == textBytes) return false;
        break;
    }
    default:
        return false;
    }
    if (words) {
        if (c.asset > assetWords || words > assetWords - c.asset) return false;
        data = assets + c.asset;
    }
    out = {c.op, c.x, c.y, c.w, c.h, c.color, c.arg, c.dx, c.dy, c.clip, data};
    return true;
}

} // namespace brook
