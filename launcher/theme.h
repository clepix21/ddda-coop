// The launcher's look in one place (owner's direction, 2026-10-29): dark, airy, cards; one accent colour
// (bordeaux) only for the main action, the selected item and active states. Every colour, size and
// duration the window uses comes from here.
#pragma once
#include <windows.h>
#include <gdiplus.h>

namespace theme {

// colours (sRGB)
constexpr COLORREF kBg = RGB(0x16, 0x18, 0x1c);            // window background
constexpr COLORREF kSidebar = RGB(0x11, 0x13, 0x16);       // the sidebar, a step darker
constexpr COLORREF kSurface = RGB(0x1f, 0x22, 0x28);       // cards, fields
constexpr COLORREF kSurfaceHover = RGB(0x26, 0x2a, 0x31);  // a card or button under the pointer
constexpr COLORREF kSurfaceRaised = RGB(0x2a, 0x2e, 0x36); // secondary buttons
constexpr COLORREF kBorder = RGB(0x2c, 0x30, 0x38);        // very light outlines
constexpr COLORREF kAccent = RGB(0x8b, 0x1e, 0x2b);        // bordeaux: main action, selection, active
constexpr COLORREF kAccentHover = RGB(0xa6, 0x2a, 0x39);
constexpr COLORREF kText = RGB(0xe6, 0xe6, 0xe6);          // main text
constexpr COLORREF kMuted = RGB(0x9a, 0xa0, 0xa6);         // secondary text
constexpr COLORREF kOk = RGB(0x5c, 0xc2, 0x7f);            // states: success, warning, error, information
constexpr COLORREF kWarn = RGB(0xe8, 0xa8, 0x48);
constexpr COLORREF kBad = RGB(0xf0, 0x6a, 0x62);
constexpr COLORREF kInfo = RGB(0x6a, 0xa8, 0xe8);
constexpr COLORREF kFocus = RGB(0xd0, 0xd4, 0xda);         // keyboard focus ring

// sizes, in 96-dpi units (scaled by px() in the launcher)
constexpr int kRadiusCard = 14, kRadiusButton = 9, kRadiusSmall = 6;
constexpr int kPadCard = 18, kGapSection = 24, kGapSmall = 12;

// type: one family, few sizes
// Roboto (owner's choice over Inter, 2026-10-30), inside the exe (launcher.rc); without it Segoe UI Variable
// Text (Windows 11), then Segoe UI
constexpr const wchar_t *kFontRoboto = L"Roboto";
constexpr const wchar_t *kFontRobotoMedium = L"Roboto Medium"; // Roboto has no SemiBold: Medium (500), its own family
constexpr const wchar_t *kFontFamily = L"Segoe UI Variable Text";
constexpr const wchar_t *kFontFallback = L"Segoe UI";
constexpr int kSizeSmall = 9, kSizeBody = 10, kSizeTitle = 13, kSizeDisplay = 20; // points

// motion
constexpr int kHoverMs = 160;

inline Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

// a colour between a and b, t in 0..1 (hover fades)
inline COLORREF mix(COLORREF a, COLORREF b, float t)
{
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    return RGB(int(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), int(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               int(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

inline void round_path(Gdiplus::GraphicsPath &p, const RECT &r, int radius)
{
    float x = float(r.left), y = float(r.top), w = float(r.right - r.left - 1), h = float(r.bottom - r.top - 1);
    float d = float(radius * 2);
    if (d > w)
        d = w;
    if (d > h)
        d = h;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// A rounded rectangle, filled and optionally outlined (border 0 = none), anti-aliased.
inline void round_rect(HDC dc, const RECT &r, int radius, COLORREF fill, COLORREF border = CLR_INVALID, float width = 1.f,
                       bool dashed = false)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    round_path(p, r, radius);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(gp(fill));
        g.FillPath(&b, &p);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(gp(border), width);
        if (dashed)
            pen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawPath(&pen, &p);
    }
}

inline void circle(HDC dc, const RECT &r, COLORREF fill)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(gp(fill));
    g.FillEllipse(&b, float(r.left), float(r.top), float(r.right - r.left - 1), float(r.bottom - r.top - 1));
}

} // namespace theme
