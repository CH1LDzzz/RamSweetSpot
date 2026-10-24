// Простая 2D-графика в framebuffer и текст системным шрифтом (кириллица есть в Standard).
#pragma once

#include "common.hpp"

namespace gfx {

constexpr int W = 1280, H = 720;

inline constexpr u32 rgb(u8 r, u8 g, u8 b) { return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u; }

// Палитра интерфейса.
namespace col {
constexpr u32 bg = rgb(0x12, 0x15, 0x1B);
constexpr u32 panel = rgb(0x1B, 0x20, 0x29);
constexpr u32 panel2 = rgb(0x23, 0x2A, 0x36);
constexpr u32 line = rgb(0x31, 0x3A, 0x48);
constexpr u32 text = rgb(0xE8, 0xEB, 0xF0);
constexpr u32 dim = rgb(0x8C, 0x97, 0xA6);
constexpr u32 accent = rgb(0xFF, 0x9F, 0x43);
constexpr u32 accentDim = rgb(0x5A, 0x3B, 0x1E);
constexpr u32 good = rgb(0x4C, 0xD0, 0x7D);
constexpr u32 bad = rgb(0xFF, 0x5C, 0x5C);
constexpr u32 warn = rgb(0xFF, 0xCC, 0x4D);
constexpr u32 info = rgb(0x5C, 0xB8, 0xFF);
constexpr u32 sel = rgb(0x2E, 0x3A, 0x4E);
}  // namespace col

bool init(std::string* err);
void exit();
#ifndef __SWITCH__
bool initHost(const char* fontPath);  // предпросмотр на ПК
bool saveFramePPM(const char* path);
#endif

void beginFrame();
void endFrame();

void clear(u32 c);
void rect(int x, int y, int w, int h, u32 c);
void frame(int x, int y, int w, int h, u32 c, int t = 1);
void hline(int x, int y, int w, u32 c);
void vline(int x, int y, int h, u32 c);
void line(int x0, int y0, int x1, int y1, u32 c);

// Текст: (x, y) — левый верх строки. Возвращает ширину. maxW > 0 — обрезать с «…».
int text(int x, int y, int size, u32 c, const std::string& s, int maxW = 0);
int textWidth(int size, const std::string& s);
void textRight(int xr, int y, int size, u32 c, const std::string& s);
void textCenter(int xc, int y, int size, u32 c, const std::string& s);
// Перенос по словам. Возвращает высоту.
int textWrap(int x, int y, int size, u32 c, const std::string& s, int maxW, int lineH);

}  // namespace gfx
