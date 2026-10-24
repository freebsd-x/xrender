#include "xrender/svg_engine.h"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <optional>

namespace fbx::xrender {
namespace {

// ── Basic types ───────────────────────────────────────────────────────────────
struct V2 { float x{0}, y{0}; };
struct RGBA { float r{0}, g{0}, b{0}, a{0}; };

struct GradStop { float offset{0}; RGBA color; };
struct Gradient {
    std::string id;
    float x1{0}, y1{0}, x2{1}, y2{0};   // objectBoundingBox fractions
    std::vector<GradStop> stops;
};

struct Paint {
    enum class Kind { None, Solid, Grad } kind{Kind::None};
    RGBA solid;
    const Gradient* grad{nullptr};
};

// ── Tiny string helpers ───────────────────────────────────────────────────────
bool isNumStart(char c) { return std::isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.'; }

// Read the attribute value name="..." (or name='...') from within a tag string.
std::optional<std::string> attr(std::string_view tag, std::string_view name) {
    // Match name followed by optional spaces + '='. Scan so "x1" doesn't match "x".
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string_view::npos) {
        // preceding char must be a delimiter (space/start), so "stroke-width"
        // isn't hit when searching for "width".
        bool okPrev = (pos == 0) || tag[pos - 1] == ' ' || tag[pos - 1] == '\t' ||
                      tag[pos - 1] == '\n' || tag[pos - 1] == '\r';
        size_t after = pos + name.size();
        size_t q = after;
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
        if (okPrev && q < tag.size() && tag[q] == '=') {
            size_t v = q + 1;
            while (v < tag.size() && (tag[v] == ' ' || tag[v] == '\t')) ++v;
            if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                char quote = tag[v];
                size_t end = tag.find(quote, v + 1);
                if (end != std::string_view::npos)
                    return std::string(tag.substr(v + 1, end - v - 1));
            }
        }
        pos = after;
    }
    return std::nullopt;
}

// Pull a sub-property out of a style="a:b;c:d" attribute.
std::optional<std::string> styleProp(const std::string& style, std::string_view key) {
    size_t pos = 0;
    while ((pos = style.find(key, pos)) != std::string::npos) {
        size_t c = pos + key.size();
        while (c < style.size() && (style[c] == ' ')) ++c;
        if (c < style.size() && style[c] == ':') {
            size_t v = c + 1;
            while (v < style.size() && style[v] == ' ') ++v;
            size_t end = style.find(';', v);
            if (end == std::string::npos) end = style.size();
            std::string val = style.substr(v, end - v);
            while (!val.empty() && (val.back() == ' ')) val.pop_back();
            return val;
        }
        pos = c;
    }
    return std::nullopt;
}

float toFloat(const std::string& s, float def = 0.0f) {
    try { return std::stof(s); } catch (...) { return def; }
}

// Parse a colour token: #rgb, #rrggbb, a few named colours. Alpha stays 1.
std::optional<RGBA> parseColor(std::string s) {
    // trim
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty() || s == "none" || s == "transparent") return std::nullopt;
    auto mk = [](int r, int g, int b) {
        return RGBA{r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    };
    if (s[0] == '#') {
        std::string h = s.substr(1);
        auto hx = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        if (h.size() == 3) return mk(hx(h[0]) * 17, hx(h[1]) * 17, hx(h[2]) * 17);
        if (h.size() >= 6) return mk(hx(h[0]) * 16 + hx(h[1]),
                                     hx(h[2]) * 16 + hx(h[3]),
                                     hx(h[4]) * 16 + hx(h[5]));
        return std::nullopt;
    }
    if (s == "white")  return mk(255, 255, 255);
    if (s == "black")  return mk(0, 0, 0);
    if (s == "red")    return mk(255, 0, 0);
    if (s == "green")  return mk(0, 128, 0);
    if (s == "blue")   return mk(0, 0, 255);
    if (s == "gray" || s == "grey") return mk(128, 128, 128);
    return std::nullopt;
}

// ── Path data flattening ──────────────────────────────────────────────────────
// A subpath is a polyline; closed subpaths are used for fill winding and closed
// strokes. Beziers and arcs are flattened to line segments here.
struct Poly { std::vector<V2> pts; bool closed{false}; };

struct NumReader {
    std::string_view s; size_t i{0};
    bool next(float& out) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' ||
                                s[i] == '\t' || s[i] == '\r')) ++i;
        if (i >= s.size() || !isNumStart(s[i])) return false;
        size_t start = i;
        if (s[i] == '+' || s[i] == '-') ++i;
        bool dot = false, exp = false;
        while (i < s.size()) {
            char c = s[i];
            if (std::isdigit((unsigned char)c)) { ++i; }
            else if (c == '.' && !dot && !exp) { dot = true; ++i; }
            else if ((c == 'e' || c == 'E') && !exp) { exp = true; ++i;
                     if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i; }
            else break;
        }
        out = toFloat(std::string(s.substr(start, i - start)));
        return true;
    }
};

void flattenCubic(V2 p0, V2 p1, V2 p2, V2 p3, std::vector<V2>& out) {
    const int N = 18;
    for (int k = 1; k <= N; ++k) {
        float t = (float)k / N, u = 1 - t;
        float a = u*u*u, b = 3*u*u*t, c = 3*u*t*t, d = t*t*t;
        out.push_back({a*p0.x + b*p1.x + c*p2.x + d*p3.x,
                       a*p0.y + b*p1.y + c*p2.y + d*p3.y});
    }
}
void flattenQuad(V2 p0, V2 p1, V2 p2, std::vector<V2>& out) {
    const int N = 14;
    for (int k = 1; k <= N; ++k) {
        float t = (float)k / N, u = 1 - t;
        float a = u*u, b = 2*u*t, c = t*t;
        out.push_back({a*p0.x + b*p1.x + c*p2.x, a*p0.y + b*p1.y + c*p2.y});
    }
}

// SVG elliptical arc (endpoint parameterization) → polyline.
void flattenArc(V2 p0, float rx, float ry, float xrotDeg, bool largeArc,
                bool sweep, V2 p1, std::vector<V2>& out) {
    if (rx == 0 || ry == 0) { out.push_back(p1); return; }
    rx = std::fabs(rx); ry = std::fabs(ry);
    float phi = xrotDeg * (float)M_PI / 180.0f;
    float cosP = std::cos(phi), sinP = std::sin(phi);
    float dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
    float x1p =  cosP*dx + sinP*dy;
    float y1p = -sinP*dx + cosP*dy;
    float rxs = rx*rx, rys = ry*ry, x1s = x1p*x1p, y1s = y1p*y1p;
    float lam = x1s/rxs + y1s/rys;
    if (lam > 1) { float s = std::sqrt(lam); rx *= s; ry *= s; rxs = rx*rx; rys = ry*ry; }
    float num = rxs*rys - rxs*y1s - rys*x1s;
    float den = rxs*y1s + rys*x1s;
    float co = std::sqrt(std::max(0.0f, num/den));
    if (largeArc == sweep) co = -co;
    float cxp =  co * (rx*y1p/ry);
    float cyp = -co * (ry*x1p/rx);
    float cx = cosP*cxp - sinP*cyp + (p0.x + p1.x)/2;
    float cy = sinP*cxp + cosP*cyp + (p0.y + p1.y)/2;
    auto ang = [](float ux, float uy, float vx, float vy) {
        float dot = ux*vx + uy*vy;
        float len = std::sqrt((ux*ux+uy*uy)*(vx*vx+vy*vy));
        float a = std::acos(std::clamp(len > 0 ? dot/len : 1.0f, -1.0f, 1.0f));
        return (ux*vy - uy*vx < 0) ? -a : a;
    };
    float th0 = ang(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry);
    float dth = ang((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry);
    if (!sweep && dth > 0) dth -= 2*(float)M_PI;
    if (sweep && dth < 0) dth += 2*(float)M_PI;
    int N = std::max(2, (int)std::ceil(std::fabs(dth) / (0.2f)));
    for (int k = 1; k <= N; ++k) {
        float th = th0 + dth * ((float)k / N);
        float ex = cosP*rx*std::cos(th) - sinP*ry*std::sin(th) + cx;
        float ey = sinP*rx*std::cos(th) + cosP*ry*std::sin(th) + cy;
        out.push_back({ex, ey});
    }
}

std::vector<Poly> parsePathData(std::string_view d) {
    std::vector<Poly> polys;
    Poly cur;
    V2 pt{0, 0}, start{0, 0};
    V2 prevCtrl{0, 0};
    char prevCmd = 0;
    NumReader r{d, 0};
    size_t i = 0;
    auto flushOpen = [&]() {
        if (cur.pts.size() >= 1) { polys.push_back(cur); }
        cur = Poly{};
    };
    while (i < d.size()) {
        char c = d[i];
        if (std::isspace((unsigned char)c) || c == ',') { ++i; continue; }
        char cmd;
        if (std::isalpha((unsigned char)c)) { cmd = c; ++i; }
        else { cmd = prevCmd; }  // implicit repeat
        r.i = i;
        bool rel = std::islower((unsigned char)cmd);
        char C = std::toupper((unsigned char)cmd);
        auto rd = [&](float& v) { bool ok = r.next(v); i = r.i; return ok; };
        if (C == 'M') {
            float x, y; if (!rd(x) || !rd(y)) break;
            if (rel) { x += pt.x; y += pt.y; }
            flushOpen();
            pt = {x, y}; start = pt;
            cur.pts.push_back(pt);
            prevCmd = rel ? 'l' : 'L';  // subsequent implicit = lineto
        } else if (C == 'L') {
            float x, y; if (!rd(x) || !rd(y)) break;
            if (rel) { x += pt.x; y += pt.y; }
            pt = {x, y}; cur.pts.push_back(pt); prevCmd = cmd;
        } else if (C == 'H') {
            float x; if (!rd(x)) break; if (rel) x += pt.x;
            pt.x = x; cur.pts.push_back(pt); prevCmd = cmd;
        } else if (C == 'V') {
            float y; if (!rd(y)) break; if (rel) y += pt.y;
            pt.y = y; cur.pts.push_back(pt); prevCmd = cmd;
        } else if (C == 'C') {
            float x1,y1,x2,y2,x,y;
            if (!rd(x1)||!rd(y1)||!rd(x2)||!rd(y2)||!rd(x)||!rd(y)) break;
            if (rel){x1+=pt.x;y1+=pt.y;x2+=pt.x;y2+=pt.y;x+=pt.x;y+=pt.y;}
            flattenCubic(pt,{x1,y1},{x2,y2},{x,y},cur.pts);
            prevCtrl={x2,y2}; pt={x,y}; prevCmd=cmd;
        } else if (C == 'S') {
            float x2,y2,x,y; if (!rd(x2)||!rd(y2)||!rd(x)||!rd(y)) break;
            if (rel){x2+=pt.x;y2+=pt.y;x+=pt.x;y+=pt.y;}
            V2 c1 = (std::toupper(prevCmd)=='C'||std::toupper(prevCmd)=='S')
                    ? V2{2*pt.x-prevCtrl.x, 2*pt.y-prevCtrl.y} : pt;
            flattenCubic(pt,c1,{x2,y2},{x,y},cur.pts);
            prevCtrl={x2,y2}; pt={x,y}; prevCmd=cmd;
        } else if (C == 'Q') {
            float x1,y1,x,y; if (!rd(x1)||!rd(y1)||!rd(x)||!rd(y)) break;
            if (rel){x1+=pt.x;y1+=pt.y;x+=pt.x;y+=pt.y;}
            flattenQuad(pt,{x1,y1},{x,y},cur.pts);
            prevCtrl={x1,y1}; pt={x,y}; prevCmd=cmd;
        } else if (C == 'T') {
            float x,y; if (!rd(x)||!rd(y)) break;
            if (rel){x+=pt.x;y+=pt.y;}
            V2 c1 = (std::toupper(prevCmd)=='Q'||std::toupper(prevCmd)=='T')
                    ? V2{2*pt.x-prevCtrl.x, 2*pt.y-prevCtrl.y} : pt;
            flattenQuad(pt,c1,{x,y},cur.pts);
            prevCtrl=c1; pt={x,y}; prevCmd=cmd;
        } else if (C == 'A') {
            float rx,ry,rot,laf,sf,x,y;
            if(!rd(rx)||!rd(ry)||!rd(rot)||!rd(laf)||!rd(sf)||!rd(x)||!rd(y)) break;
            if (rel){x+=pt.x;y+=pt.y;}
            flattenArc(pt,rx,ry,rot,laf!=0,sf!=0,{x,y},cur.pts);
            pt={x,y}; prevCmd=cmd;
        } else if (C == 'Z') {
            cur.closed = true;
            if (!cur.pts.empty()) polys.push_back(cur);
            cur = Poly{};
            pt = start;
            prevCmd = cmd;
        } else {
            ++i; // unknown command, skip
        }
    }
    flushOpen();
    return polys;
}

// Rounded-rect outline (one closed polygon).
Poly roundedRect(float x, float y, float w, float h, float rx, float ry) {
    Poly p; p.closed = true;
    rx = std::min(rx, w/2); ry = std::min(ry, h/2);
    if (rx <= 0 || ry <= 0) {
        p.pts = {{x,y},{x+w,y},{x+w,y+h},{x,y+h}};
        return p;
    }
    const int N = 6;
    auto arc = [&](float cx, float cy, float a0, float a1) {
        for (int k = 0; k <= N; ++k) {
            float a = a0 + (a1-a0)*((float)k/N);
            p.pts.push_back({cx + rx*std::cos(a), cy + ry*std::sin(a)});
        }
    };
    arc(x+w-rx, y+ry,   -(float)M_PI/2, 0);              // top-right
    arc(x+w-rx, y+h-ry,  0,             (float)M_PI/2);  // bottom-right
    arc(x+rx,   y+h-ry,  (float)M_PI/2, (float)M_PI);    // bottom-left
    arc(x+rx,   y+ry,    (float)M_PI,   3*(float)M_PI/2);// top-left
    return p;
}

// ── Rasterization ─────────────────────────────────────────────────────────────
struct Canvas {
    int W, H;
    std::vector<float> buf; // premultiplied RGBA, row-major
    Canvas(int w, int h) : W(w), H(h), buf((size_t)w*h*4, 0.0f) {}
    void over(int x, int y, RGBA c, float cov) {
        if (x < 0 || y < 0 || x >= W || y >= H || cov <= 0) return;
        float sa = c.a * cov;
        if (sa <= 0) return;
        size_t idx = ((size_t)y*W + x)*4;
        float ia = 1 - sa;
        buf[idx+0] = c.r*sa + buf[idx+0]*ia;
        buf[idx+1] = c.g*sa + buf[idx+1]*ia;
        buf[idx+2] = c.b*sa + buf[idx+2]*ia;
        buf[idx+3] = sa      + buf[idx+3]*ia;
    }
};

int windingAt(const std::vector<Poly>& polys, float px, float py) {
    int wind = 0;
    for (const auto& poly : polys) {
        size_t n = poly.pts.size();
        if (n < 2) continue;
        for (size_t k = 0; k < n; ++k) {
            V2 a = poly.pts[k];
            V2 b = poly.pts[(k+1)%n];   // close implicitly for fill
            if ((a.y <= py) != (b.y <= py)) {
                float t = (py - a.y) / (b.y - a.y);
                float xc = a.x + t*(b.x - a.x);
                if (px < xc) wind += (b.y > a.y) ? 1 : -1;
            }
        }
    }
    return wind;
}

float segDist2(float px, float py, V2 a, V2 b) {
    float vx = b.x-a.x, vy = b.y-a.y;
    float wx = px-a.x, wy = py-a.y;
    float len2 = vx*vx + vy*vy;
    float t = len2 > 0 ? std::clamp((wx*vx + wy*vy)/len2, 0.0f, 1.0f) : 0.0f;
    float dx = px - (a.x + t*vx), dy = py - (a.y + t*vy);
    return dx*dx + dy*dy;
}

RGBA gradColor(const Gradient& g, float px, float py,
               float bx, float by, float bw, float bh) {
    if (g.stops.empty()) return RGBA{0,0,0,0};
    if (g.stops.size() == 1) return g.stops[0].color;
    float sx = bx + g.x1*bw, sy = by + g.y1*bh;
    float ex = bx + g.x2*bw, ey = by + g.y2*bh;
    float dx = ex-sx, dy = ey-sy;
    float len2 = dx*dx + dy*dy;
    float t = len2 > 0 ? ((px-sx)*dx + (py-sy)*dy)/len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    // find surrounding stops
    const GradStop* lo = &g.stops.front();
    const GradStop* hi = &g.stops.back();
    for (size_t k = 0; k+1 < g.stops.size(); ++k) {
        if (t >= g.stops[k].offset && t <= g.stops[k+1].offset) {
            lo = &g.stops[k]; hi = &g.stops[k+1]; break;
        }
    }
    float span = hi->offset - lo->offset;
    float f = span > 1e-6f ? (t - lo->offset)/span : 0.0f;
    return RGBA{ lo->color.r + (hi->color.r-lo->color.r)*f,
                 lo->color.g + (hi->color.g-lo->color.g)*f,
                 lo->color.b + (hi->color.b-lo->color.b)*f,
                 lo->color.a + (hi->color.a-lo->color.a)*f };
}

// bounding box of a set of polys
void bbox(const std::vector<Poly>& polys, float& bx, float& by, float& bw, float& bh) {
    float minx=1e30f, miny=1e30f, maxx=-1e30f, maxy=-1e30f;
    for (auto& p : polys) for (auto& v : p.pts) {
        minx=std::min(minx,v.x); miny=std::min(miny,v.y);
        maxx=std::max(maxx,v.x); maxy=std::max(maxy,v.y);
    }
    if (maxx < minx) { bx=by=0; bw=bh=1; return; }
    bx=minx; by=miny; bw=std::max(1e-3f,maxx-minx); bh=std::max(1e-3f,maxy-miny);
}

constexpr int SS = 4; // supersampling factor per axis

void fillPolys(Canvas& cv, const std::vector<Poly>& polys, const Paint& paint,
               float opacity) {
    if (paint.kind == Paint::Kind::None) return;
    float bx,by,bw,bh; bbox(polys, bx,by,bw,bh);
    // pixel bounds
    int minX = std::max(0, (int)std::floor(bx));
    int minY = std::max(0, (int)std::floor(by));
    int maxX = std::min(cv.W-1, (int)std::ceil(bx+bw));
    int maxY = std::min(cv.H-1, (int)std::ceil(by+bh));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            int inside = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    float fx = x + (sx+0.5f)/SS, fy = y + (sy+0.5f)/SS;
                    if (windingAt(polys, fx, fy) != 0) ++inside;
                }
            if (!inside) continue;
            float cov = (float)inside/(SS*SS);
            RGBA col = paint.kind == Paint::Kind::Grad
                ? gradColor(*paint.grad, x+0.5f, y+0.5f, bx,by,bw,bh)
                : paint.solid;
            cv.over(x, y, col, cov * opacity);
        }
    }
}

void strokePolys(Canvas& cv, const std::vector<Poly>& polys, const Paint& paint,
                 float width, float opacity) {
    if (paint.kind == Paint::Kind::None || width <= 0) return;
    float half = width/2;
    float h2 = half*half;
    float bx,by,bw,bh; bbox(polys, bx,by,bw,bh);
    int minX = std::max(0, (int)std::floor(bx-half));
    int minY = std::max(0, (int)std::floor(by-half));
    int maxX = std::min(cv.W-1, (int)std::ceil(bx+bw+half));
    int maxY = std::min(cv.H-1, (int)std::ceil(by+bh+half));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            int inside = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    float fx = x + (sx+0.5f)/SS, fy = y + (sy+0.5f)/SS;
                    float best = 1e30f;
                    for (const auto& poly : polys) {
                        size_t n = poly.pts.size();
                        size_t segs = poly.closed ? n : (n>0?n-1:0);
                        for (size_t k = 0; k < segs; ++k)
                            best = std::min(best, segDist2(fx, fy, poly.pts[k],
                                             poly.pts[(k+1)%n]));
                    }
                    if (best <= h2) ++inside;
                }
            if (!inside) continue;
            float cov = (float)inside/(SS*SS);
            RGBA col = paint.kind == Paint::Kind::Grad
                ? gradColor(*paint.grad, x+0.5f, y+0.5f, bx,by,bw,bh)
                : paint.solid;
            cv.over(x, y, col, cov * opacity);
        }
    }
}

// ── Gradient + paint resolution ───────────────────────────────────────────────
float parseCoord(const std::string& s, float def) {
    if (s.empty()) return def;
    if (s.back() == '%') return toFloat(s.substr(0, s.size()-1)) / 100.0f;
    return toFloat(s, def);
}

std::vector<Gradient> parseGradients(std::string_view svg) {
    std::vector<Gradient> grads;
    size_t pos = 0;
    while ((pos = svg.find("<linearGradient", pos)) != std::string_view::npos) {
        size_t tagEnd = svg.find('>', pos);
        size_t blockEnd = svg.find("</linearGradient", pos);
        if (tagEnd == std::string_view::npos || blockEnd == std::string_view::npos) break;
        std::string_view tag = svg.substr(pos, tagEnd - pos);
        Gradient g;
        if (auto v = attr(tag, "id")) g.id = *v;
        if (auto v = attr(tag, "x1")) g.x1 = parseCoord(*v, 0);
        if (auto v = attr(tag, "y1")) g.y1 = parseCoord(*v, 0);
        if (auto v = attr(tag, "x2")) g.x2 = parseCoord(*v, 1);
        if (auto v = attr(tag, "y2")) g.y2 = parseCoord(*v, 0);
        // stops
        std::string_view body = svg.substr(tagEnd, blockEnd - tagEnd);
        size_t sp = 0;
        while ((sp = body.find("<stop", sp)) != std::string_view::npos) {
            size_t se = body.find('>', sp);
            if (se == std::string_view::npos) break;
            std::string_view st = body.substr(sp, se - sp);
            GradStop stop;
            stop.offset = parseCoord(attr(st, "offset").value_or("0"), 0);
            std::string style = attr(st, "style").value_or("");
            std::string colStr = styleProp(style, "stop-color")
                                     .value_or(attr(st, "stop-color").value_or("black"));
            RGBA c = parseColor(colStr).value_or(RGBA{0,0,0,1});
            std::string opStr = styleProp(style, "stop-opacity")
                                     .value_or(attr(st, "stop-opacity").value_or("1"));
            c.a = toFloat(opStr, 1.0f);
            stop.color = c;
            g.stops.push_back(stop);
            sp = se;
        }
        std::sort(g.stops.begin(), g.stops.end(),
                  [](const GradStop& a, const GradStop& b){ return a.offset < b.offset; });
        grads.push_back(std::move(g));
        pos = blockEnd;
    }
    return grads;
}

Paint resolvePaint(const std::string& spec, const std::vector<Gradient>& grads) {
    Paint p;
    if (spec.empty() || spec == "none") return p;
    if (spec.rfind("url(", 0) == 0) {
        size_t h = spec.find('#');
        size_t e = spec.find(')', h);
        if (h != std::string::npos && e != std::string::npos) {
            std::string id = spec.substr(h+1, e-h-1);
            for (const auto& g : grads)
                if (g.id == id) { p.kind = Paint::Kind::Grad; p.grad = &g; return p; }
        }
        return p;
    }
    if (auto c = parseColor(spec)) { p.kind = Paint::Kind::Solid; p.solid = *c; }
    return p;
}

} // namespace

std::expected<ImageData, std::error_code>
SvgEngine::decode(std::string_view svg, uint32_t target_w, uint32_t target_h) noexcept {
    ImageData img;
    img.width_ = target_w;
    img.height_ = target_h;
    img.pixels_.assign((size_t)target_w * target_h * 4, 0); // transparent

    // viewBox → user-space extents (default to 0 0 100 100, our asset convention).
    float vbx = 0, vby = 0, vbw = 100, vbh = 100;
    if (auto vb = attr(svg.substr(0, std::min<size_t>(svg.size(), svg.find('>')+1)), "viewBox")) {
        NumReader r{*vb, 0}; float a,b,c,d;
        if (r.next(a)&&r.next(b)&&r.next(c)&&r.next(d) && c>0 && d>0)
            { vbx=a; vby=b; vbw=c; vbh=d; }
    }
    float sx = target_w / vbw, sy = target_h / vbh;
    float savg = (sx + sy) / 2;
    auto toPix = [&](Poly p){ for (auto& v : p.pts){ v.x=(v.x-vbx)*sx; v.y=(v.y-vby)*sy; } return p; };

    std::vector<Gradient> grads = parseGradients(svg);
    Canvas cv((int)target_w, (int)target_h);

    // Walk <rect> and <path> elements in document order and paint each.
    size_t i = 0;
    while (i < svg.size()) {
        size_t lt = svg.find('<', i);
        if (lt == std::string_view::npos) break;
        bool isRect = svg.compare(lt, 5, "<rect") == 0;
        bool isPath = svg.compare(lt, 5, "<path") == 0;
        if (!isRect && !isPath) { i = lt + 1; continue; }
        size_t gt = svg.find('>', lt);
        if (gt == std::string_view::npos) break;
        std::string_view tag = svg.substr(lt, gt - lt);
        i = gt + 1;

        std::string style   = attr(tag, "style").value_or("");
        std::string fillStr = styleProp(style, "fill").value_or(attr(tag, "fill").value_or("black"));
        std::string strokeStr = styleProp(style, "stroke").value_or(attr(tag, "stroke").value_or(""));
        float fillOp   = toFloat(styleProp(style, "fill-opacity").value_or(
                                 attr(tag, "fill-opacity").value_or("1")), 1.0f);
        float strokeOp = toFloat(styleProp(style, "stroke-opacity").value_or(
                                 attr(tag, "stroke-opacity").value_or("1")), 1.0f);
        float strokeW  = toFloat(styleProp(style, "stroke-width").value_or(
                                 attr(tag, "stroke-width").value_or("0")), 0.0f);

        std::vector<Poly> polys;
        if (isRect) {
            float x = toFloat(attr(tag, "x").value_or("0"));
            float y = toFloat(attr(tag, "y").value_or("0"));
            float w = toFloat(attr(tag, "width").value_or("0"));
            float h = toFloat(attr(tag, "height").value_or("0"));
            float rx = toFloat(attr(tag, "rx").value_or(attr(tag, "ry").value_or("0")));
            float ry = toFloat(attr(tag, "ry").value_or(attr(tag, "rx").value_or("0")));
            polys.push_back(roundedRect(x, y, w, h, rx, ry));
        } else {
            std::string d = attr(tag, "d").value_or("");
            polys = parsePathData(d);
        }
        for (auto& p : polys) p = toPix(p);

        Paint fillP = resolvePaint(fillStr, grads);
        fillPolys(cv, polys, fillP, fillOp);

        Paint strokeP = resolvePaint(strokeStr, grads);
        if (strokeP.kind != Paint::Kind::None && strokeW > 0)
            strokePolys(cv, polys, strokeP, strokeW * savg, strokeOp);
    }

    // Un-premultiply into straight RGBA8.
    for (size_t p = 0; p < (size_t)target_w*target_h; ++p) {
        float a = cv.buf[p*4+3];
        uint8_t A = (uint8_t)std::clamp(a*255.0f, 0.0f, 255.0f);
        auto comp = [&](float c){
            float s = a > 1e-4f ? c/a : 0.0f;
            return (uint8_t)std::clamp(s*255.0f, 0.0f, 255.0f);
        };
        img.pixels_[p*4+0] = comp(cv.buf[p*4+0]);
        img.pixels_[p*4+1] = comp(cv.buf[p*4+1]);
        img.pixels_[p*4+2] = comp(cv.buf[p*4+2]);
        img.pixels_[p*4+3] = A;
    }
    return img;
}

} // namespace fbx::xrender
