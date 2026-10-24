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

const Glyph& glyph(u32 cp, int size) {
    u64 key = ((u64)cp << 8) | (u64)(size & 0xFF);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Glyph g;
    int gi = 0;
    Font* f = fontFor(cp, &gi);
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
