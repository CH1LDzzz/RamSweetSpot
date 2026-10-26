#include "gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"
#pragma GCC diagnostic pop

namespace gfx {

namespace {
#ifdef __SWITCH__
Framebuffer g_fb;
#endif
u32* g_px = nullptr;
u32 g_stride = W;  // в пикселях

struct Font {
    bool ok = false;
    stbtt_fontinfo info;
    int ascent = 0, descent = 0, gap = 0;
};
Font g_std, g_ext;

struct Glyph {
    int w = 0, h = 0, xoff = 0, yoff = 0;
    float adv = 0;
    std::vector<u8> bmp;
};
std::unordered_map<u64, Glyph> g_cache;

Font* fontFor(u32 cp, int* glyph) {
    if (g_std.ok) {
        int g = stbtt_FindGlyphIndex(&g_std.info, (int)cp);
        if (g) {
            *glyph = g;
            return &g_std;
        }
    }
    if (g_ext.ok) {
        int g = stbtt_FindGlyphIndex(&g_ext.info, (int)cp);
        if (g) {
            *glyph = g;
            return &g_ext;
        }
    }
    *glyph = 0;
    return g_std.ok ? &g_std : nullptr;
}

// Значки, которых нет в системном шрифте Switch (на ПК — есть, поэтому в предпросмотре их не видно): рисуем сами.
// Иначе вместо «●», стрелок и галочек на консоли выходил «?».
static float segDist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l = vx * vx + vy * vy;
    float t = l > 0 ? (wx * vx + wy * vy) / l : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

static bool synthGlyph(u32 cp, int size, Glyph& g) {
    enum { None, Dot, BigDot, Up, Down, Left, Right, Check, Cross, Warn } kind = None;
    switch (cp) {
        case 0x26A0: kind = Warn; break;    // ⚠
        case 0x25CF: kind = BigDot; break;  // ●
        case 0x2022: kind = Dot; break;     // •
        case 0x2191: kind = Up; break;      // ↑
        case 0x2193: kind = Down; break;    // ↓
        case 0x2190: kind = Left; break;    // ←
        case 0x2192: kind = Right; break;   // →
        case 0x2713:
        case 0x2714: kind = Check; break;   // ✓ ✔
        case 0x2717:
        case 0x2718: kind = Cross; break;   // ✗ ✘
        default: return false;
    }
    int n = std::max(6, (int)std::lround(size * 0.62));
    g.w = n;
    g.h = n;
    g.xoff = (int)std::lround(size * 0.06);
    g.adv = n + size * 0.18f;
    float baseScale = g_std.ok ? stbtt_ScaleForPixelHeight(&g_std.info, (float)size) : 1.0f;
    int baseline = g_std.ok ? (int)std::lround(g_std.ascent * baseScale) : (int)(size * 0.8);
    g.yoff = baseline - (int)std::lround(size * 0.36) - n / 2;
    g.bmp.assign((size_t)n * n, 0);
    const float th = std::max(1.2f, n * 0.13f);  // толщина линий
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
                    float cx = n / 2.0f, cy = n / 2.0f;
                    bool in = false;
                    switch (kind) {
                        case BigDot: in = (px - cx) * (px - cx) + (py - cy) * (py - cy) <= (n * 0.42f) * (n * 0.42f); break;
                        case Dot: in = (px - cx) * (px - cx) + (py - cy) * (py - cy) <= (n * 0.22f) * (n * 0.22f); break;
                        case Up:
                        case Down:
                        case Left:
                        case Right: {
                            // Стрелка вверх в своей системе координат, остальные — поворотом.
                            float ux = px, uy = py;
                            if (kind == Down) uy = n - py;
                            if (kind == Left) {
                                ux = py;
                                uy = px;
                            }
                            if (kind == Right) {
                                ux = py;
                                uy = n - px;
                            }
                            float head = n * 0.5f;
                            bool tri = uy <= head && std::fabs(ux - cx) <= (uy / head) * (n * 0.45f);
                            bool stem = uy >= head * 0.6f && uy <= n * 0.98f && std::fabs(ux - cx) <= th * 0.7f;
                            in = tri || stem;
                            break;
                        }
                        case Check:
                            in = segDist(px, py, n * 0.08f, n * 0.55f, n * 0.38f, n * 0.85f) <= th ||
                                 segDist(px, py, n * 0.38f, n * 0.85f, n * 0.92f, n * 0.15f) <= th;
                            break;
                        case Cross:
                            in = segDist(px, py, n * 0.15f, n * 0.15f, n * 0.85f, n * 0.85f) <= th ||
                                 segDist(px, py, n * 0.85f, n * 0.15f, n * 0.15f, n * 0.85f) <= th;
                            break;
                        case Warn:
                            // Треугольник контуром и восклицательный знак внутри.
                            in = segDist(px, py, cx, n * 0.04f, n * 0.03f, n * 0.95f) <= th * 0.8f ||
                                 segDist(px, py, n * 0.03f, n * 0.95f, n * 0.97f, n * 0.95f) <= th * 0.8f ||
                                 segDist(px, py, n * 0.97f, n * 0.95f, cx, n * 0.04f) <= th * 0.8f ||
                                 segDist(px, py, cx, n * 0.36f, cx, n * 0.64f) <= th * 0.75f ||
                                 (px - cx) * (px - cx) + (py - n * 0.80f) * (py - n * 0.80f) <= th * th;
                            break;
                        default: break;
                    }
                    if (in) hit++;
                }
            g.bmp[(size_t)y * n + x] = (u8)(hit * 255 / 16);
        }
    return true;
}

const Glyph& glyph(u32 cp, int size) {
    u64 key = ((u64)cp << 8) | (u64)(size & 0xFF);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Glyph g;
    int gi = 0;
    Font* f = fontFor(cp, &gi);
#ifdef RAMSS_FORCE_SYNTH
    gi = synthGlyph(cp, size, g) ? 0 : gi;  // предпросмотр: свои значки, как будто в шрифте их нет
    if (!gi && !g.bmp.empty()) {
        auto res = g_cache.emplace(key, std::move(g));
        return res.first->second;
    }
#endif
    if (!gi && synthGlyph(cp, size, g)) {
        auto res = g_cache.emplace(key, std::move(g));
        return res.first->second;
    }
    if (!gi && cp != '?') {
        // Нет символа ни в одном шрифте — рисуем «?».
        const Glyph& q = glyph('?', size);
        g_cache[key] = q;
        return g_cache[key];
    }
    if (f) {
        float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        g.adv = adv * scale;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&f->info, gi, scale, scale, &x0, &y0, &x1, &y1);
        g.w = x1 - x0;
        g.h = y1 - y0;
        g.xoff = x0;
        // Базовая линия по основному шрифту, чтобы символы из разных шрифтов стояли ровно.
        float baseScale = stbtt_ScaleForPixelHeight(&g_std.info, (float)size);
        g.yoff = (int)std::lround(g_std.ascent * baseScale) + y0;
        if (g.w > 0 && g.h > 0) {
            g.bmp.resize((size_t)g.w * g.h);
            stbtt_MakeGlyphBitmap(&f->info, g.bmp.data(), g.w, g.h, g.w, scale, scale, gi);
        }
    }
    auto res = g_cache.emplace(key, std::move(g));
    return res.first->second;
}

inline void blend(u32* p, u32 c, u32 a) {
    if (a == 0) return;
    if (a >= 255) {
        *p = c;
        return;
    }
    u32 d = *p;
    u32 inv = 255 - a;
    u32 r = ((c & 0xFF) * a + (d & 0xFF) * inv) / 255;
    u32 g = (((c >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * inv) / 255;
    u32 b = (((c >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * inv) / 255;
    *p = r | (g << 8) | (b << 16) | 0xFF000000u;
}

// UTF-8 -> кодовые точки.
std::vector<u32> decode(const std::string& s) {
    std::vector<u32> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        u8 c = (u8)s[i];
        u32 cp;
        int n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 6) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c >> 4) == 14) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c >> 3) == 30) {
            cp = c & 0x07;
            n = 4;
        } else {
            i++;
            continue;
        }
        if (i + n > s.size()) break;
        for (int k = 1; k < n; k++) cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

bool loadFont(Font& f, const void* data) {
    const u8* d = (const u8*)data;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || !stbtt_InitFont(&f.info, d, off)) return false;
    stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.gap);
    f.ok = true;
    return true;
}
}  // namespace

bool init(std::string* err) {
#ifdef __SWITCH__
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        if (err) *err = strf("pl: 0x%X", rc);
        return false;
    }
    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) loadFont(g_std, fd.address);
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_NintendoExt))) loadFont(g_ext, fd.address);
    if (!g_std.ok) {
        if (err) *err = "системный шрифт недоступен";
        return false;
    }
    rc = framebufferCreate(&g_fb, nwindowGetDefault(), W, H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        if (err) *err = strf("framebuffer: 0x%X", rc);
        return false;
    }
    framebufferMakeLinear(&g_fb);
    return true;
#else
    (void)err;
    return false;
#endif
}

#ifndef __SWITCH__
// Предпросмотр на ПК: рисуем в память шрифтом из файла и сохраняем PPM.
static std::vector<u32> g_host;
static std::string g_hostFont;
bool initHost(const char* fontPath) {
    if (!readFile(fontPath, g_hostFont)) return false;
    if (!loadFont(g_std, g_hostFont.data())) return false;
    g_host.assign((size_t)W * H, 0);
    return true;
}
bool saveFramePPM(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (u32 v : g_host) {
        u8 px[3] = {(u8)(v & 0xFF), (u8)((v >> 8) & 0xFF), (u8)((v >> 16) & 0xFF)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    return true;
}
#endif

void exit() {
#ifdef __SWITCH__
    framebufferClose(&g_fb);
    plExit();
#endif
    g_cache.clear();
}

void beginFrame() {
#ifdef __SWITCH__
    u32 strideBytes = 0;
    g_px = (u32*)framebufferBegin(&g_fb, &strideBytes);
    g_stride = strideBytes / 4;
#else
    g_px = g_host.empty() ? nullptr : g_host.data();
    g_stride = W;
#endif
}

void endFrame() {
#ifdef __SWITCH__
    framebufferEnd(&g_fb);
#endif
    g_px = nullptr;
}

void setTheme(int theme) {
    // Порядок: bg, panel, panel2, line, text, dim, accent, accentDim, good, bad, warn, info, sel.
    static const u32 kThemes[2][13] = {
        {rgb(0x12, 0x15, 0x1B), rgb(0x1B, 0x20, 0x29), rgb(0x23, 0x2A, 0x36), rgb(0x31, 0x3A, 0x48), rgb(0xE8, 0xEB, 0xF0), rgb(0x8C, 0x97, 0xA6),
         rgb(0xFF, 0x9F, 0x43), rgb(0x5A, 0x3B, 0x1E), rgb(0x4C, 0xD0, 0x7D), rgb(0xFF, 0x5C, 0x5C), rgb(0xFF, 0xCC, 0x4D), rgb(0x5C, 0xB8, 0xFF),
         rgb(0x2E, 0x3A, 0x4E)},
        // Алая: алые панели и акцент; сбои — оранжевым, чтобы не сливаться с акцентом.
        {rgb(0x1E, 0x08, 0x0E), rgb(0x36, 0x0D, 0x1A), rgb(0x45, 0x12, 0x22), rgb(0x6B, 0x1C, 0x33), rgb(0xFF, 0xEE, 0xF1), rgb(0xD0, 0x9A, 0xA8),
         rgb(0xFF, 0x3B, 0x5C), rgb(0x7A, 0x16, 0x2C), rgb(0x6B, 0xE0, 0x95), rgb(0xFF, 0xA1, 0x3D), rgb(0xFF, 0xD7, 0x5E), rgb(0x9C, 0xC8, 0xFF),
         rgb(0x5A, 0x16, 0x2C)},
    };
    const u32* p = kThemes[theme == 1 ? 1 : 0];
    u32* dst[13] = {&col::bg, &col::panel, &col::panel2, &col::line, &col::text, &col::dim, &col::accent,
                    &col::accentDim, &col::good, &col::bad, &col::warn, &col::info, &col::sel};
    for (int i = 0; i < 13; i++) *dst[i] = p[i];
}

void clear(u32 c) { rect(0, 0, W, H, c); }

void rect(int x, int y, int w, int h, u32 c) {
    if (!g_px) return;
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    for (int yy = y0; yy < y1; yy++) {
        u32* row = g_px + (size_t)yy * g_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void frame(int x, int y, int w, int h, u32 c, int t) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c);
    rect(x + w - t, y, t, h, c);
}

void hline(int x, int y, int w, u32 c) { rect(x, y, w, 1, c); }
void vline(int x, int y, int h, u32 c) { rect(x, y, 1, h, c); }

void line(int x0, int y0, int x1, int y1, u32 c) {
    if (!g_px) return;
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        rect(x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int textWidth(int size, const std::string& s) {
    float w = 0;
    for (u32 cp : decode(s)) w += glyph(cp, size).adv;
    return (int)std::lround(w);
}

int text(int x, int y, int size, u32 c, const std::string& s, int maxW) {
    std::vector<u32> cps = decode(s);
    if (maxW > 0 && textWidth(size, s) > maxW) {
        float ell = glyph(0x2026, size).adv;
        float w = 0;
        size_t keep = 0;
        for (; keep < cps.size(); keep++) {
            float a = glyph(cps[keep], size).adv;
            if (w + a + ell > maxW) break;
            w += a;
        }
        cps.resize(keep);
        cps.push_back(0x2026);
    }
    float pen = (float)x;
    for (u32 cp : cps) {
        const Glyph& g = glyph(cp, size);
        if (g_px && !g.bmp.empty()) {
            int gx = (int)std::lround(pen) + g.xoff, gy = y + g.yoff;
            for (int yy = 0; yy < g.h; yy++) {
                int py = gy + yy;
                if (py < 0 || py >= H) continue;
                u32* row = g_px + (size_t)py * g_stride;
                const u8* src = g.bmp.data() + (size_t)yy * g.w;
                for (int xx = 0; xx < g.w; xx++) {
                    int px = gx + xx;
                    if (px < 0 || px >= W) continue;
                    blend(row + px, c, src[xx]);
                }
            }
        }
        pen += g.adv;
    }
    return (int)std::lround(pen - x);
}

void textRight(int xr, int y, int size, u32 c, const std::string& s) { text(xr - textWidth(size, s), y, size, c, s); }
void textCenter(int xc, int y, int size, u32 c, const std::string& s) { text(xc - textWidth(size, s) / 2, y, size, c, s); }

std::vector<std::string> wrapLines(int size, const std::string& s, int maxW) {
    std::vector<std::string> out;
    for (auto& para : split(s, '\n')) {
        std::string cur;
        for (auto& word : split(para, ' ')) {
            std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && textWidth(size, cand) > maxW) {
                out.push_back(cur);
                cur = word;
            } else {
                cur = cand;
            }
        }
        out.push_back(cur);
    }
    return out;
}

int textScroll(int x, int y, int size, u32 c, const std::string& s, int maxW, int lineH, int maxH, int* scroll) {
    const int barW = 6;
    auto lines = wrapLines(size, s, maxW - barW - 8);
    int total = (int)lines.size();
    int visible = std::max(1, maxH / lineH);
    int maxScroll = std::max(0, total - visible);
    int sc = scroll ? std::max(0, std::min(*scroll, maxScroll)) : 0;
    if (scroll) *scroll = sc;
    for (int i = 0; i < visible && sc + i < total; i++) text(x, y + i * lineH, size, c, lines[sc + i], maxW - barW - 8);
    if (maxScroll > 0) {
        // Полоса прокрутки: дорожка и ползунок по доле видимого.
        int bx = x + maxW - barW, bh = visible * lineH;
        rect(bx, y, barW, bh, col::line);
        int th = std::max(16, bh * visible / total);
        int ty = y + (bh - th) * sc / maxScroll;
        rect(bx, ty, barW, th, col::accent);
    }
    return total;
}

int textWrap(int x, int y, int size, u32 c, const std::string& s, int maxW, int lineH) {
    int yy = y;
    for (auto& para : split(s, '\n')) {
        std::string cur;
        for (auto& word : split(para, ' ')) {
            std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && textWidth(size, cand) > maxW) {
                text(x, yy, size, c, cur);
                yy += lineH;
                cur = word;
            } else {
                cur = cand;
            }
        }
        text(x, yy, size, c, cur);
        yy += lineH;
    }
    return yy - y;
}

}  // namespace gfx
