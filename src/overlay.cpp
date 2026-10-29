// "Mod active" badge (owner's request, 2026-10-29): on the title screen and its main menu, a small
// banner at the top right says the co-op mod is loaded, this PC's role and how many players are in.
//
// Drawn in IDirect3DDevice9::Present (vtable slot 17, the address taken from a throwaway device made
// on a hidden window): the text is rendered once with GDI into a 32-bit DIB, copied into a managed
// texture (alpha from the white text over black), and drawn as one screen quad. The game's states are
// kept in a state block made and freed around the draw (state blocks must not live across a Reset);
// the managed texture survives a Reset.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr int kTexW = 2048, kTexH = 128;
using Present_t = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
Present_t g_present_orig = nullptr;
IDirect3DDevice9 *g_tex_dev = nullptr;
IDirect3DTexture9 *g_tex = nullptr;
wchar_t g_tex_text[160] = L"";
int g_text_w = 0, g_text_h = 0; // pixels used in the texture

struct Vtx {
    float x, y, z, rhw, u, v;
};

// What the badge says now, or false when it is not shown (in game: only on the title screen).
bool badge_text(wchar_t *out, size_t cap)
{
    uintptr_t area = game::at<uintptr_t>(game::kArea, 0);
    uintptr_t cur = area ? game::at<uintptr_t>(area, game::kAreaCurrent) : 0;
    const char *cls = cur ? enemies_class_of(cur) : "";
    static char last[48] = "";
    if (strncmp(cls, last, sizeof last - 1) != 0) {
        strncpy(last, cls, sizeof last - 1);
        logf("overlay: area %s", cls);
    }
    if (strncmp(cls, "aTitle", 6) != 0)
        return false;
    NetRole role = net_role();
    int players = 0;
    for (int s = 0; s < 4; ++s) {
        PlayerInfo p;
        players += net_player_info(s, p) && p.present;
    }
    const wchar_t *who = role == NET_HOST ? L"hôte" : role == NET_GUEST ? L"invité" : L"seul";
    if (role == NET_OFF)
        _snwprintf(out, cap, L"DDDA Coop actif  ·  seul (pas de partie en réseau)");
    else
        _snwprintf(out, cap, L"DDDA Coop actif  ·  %ls%ls  ·  %d joueur%ls", who, net_shared() ? L", monde partagé" : L"",
                   players < 1 ? 1 : players, players > 1 ? L"s" : L"");
    out[cap - 1] = 0;
    return true;
}

// The text into the texture: a dark rounded panel, a green dot, white text.
bool render_text(IDirect3DDevice9 *dev, const wchar_t *text)
{
    if (g_tex && g_tex_dev == dev && wcscmp(text, g_tex_text) == 0)
        return true;
    if (g_tex && g_tex_dev != dev) { // another device (never seen in DDDA): start again
        g_tex->Release();
        g_tex = nullptr;
    }
    if (!g_tex && FAILED(dev->CreateTexture(kTexW, kTexH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
        g_tex = nullptr;
        return false;
    }
    g_tex_dev = dev;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kTexW, -kTexH, 1, 32, BI_RGB};
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-44, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text, int(wcslen(text)), &sz);
    int pad = 22, dot = 18;
    g_text_w = sz.cx + pad * 3 + dot;
    if (g_text_w > kTexW)
        g_text_w = kTexW;
    g_text_h = sz.cy + pad;
    if (g_text_h > kTexH)
        g_text_h = kTexH;
    // panel, dot and text in separate channels: red = panel, green = dot, blue = text coverage
    memset(bits, 0, size_t(kTexW) * kTexH * 4);
    HBRUSH panel = CreateSolidBrush(RGB(255, 0, 0));
    HGDIOBJ old_brush = SelectObject(dc, panel);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, 0, 0, g_text_w, g_text_h, 24, 24);
    HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
    SelectObject(dc, green);
    int dy = (g_text_h - dot) / 2;
    Ellipse(dc, pad, dy, pad + dot, dy + dot);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 255));
    TextOutW(dc, pad * 2 + dot, pad / 2, text, int(wcslen(text)));
    GdiFlush();
    D3DLOCKED_RECT lr;
    bool ok = SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0));
    if (ok) {
        const uint8_t *src = static_cast<const uint8_t *>(bits);
        for (int y = 0; y < kTexH; ++y) {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < kTexW; ++x) {
                const uint8_t *p = src + (size_t(y) * kTexW + x) * 4; // B G R x
                unsigned t = p[0], g = p[1], r = p[2];
                // over the panel (20, 24, 30 at 80 %): the dot green (90, 200, 120), the text white
                unsigned a = r * 205 / 255, cr = 20, cg = 24, cb = 30;
                if (g > r) {
                    a = 255;
                    cr = 90, cg = 200, cb = 120;
                }
                if (t) {
                    unsigned k = t;
                    cr = (cr * (255 - k) + 255 * k) / 255;
                    cg = (cg * (255 - k) + 255 * k) / 255;
                    cb = (cb * (255 - k) + 255 * k) / 255;
                    a = a > k ? a : k;
                }
                row[x] = (a << 24) | (cr << 16) | (cg << 8) | cb;
            }
        }
        g_tex->UnlockRect(0);
        wcsncpy(g_tex_text, text, 159);
        g_tex_text[159] = 0;
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(green);
    DeleteObject(panel);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}

void draw_badge(IDirect3DDevice9 *dev)
{
    static DWORD next_text = 0;
    static wchar_t text[160] = L"";
    static bool shown = false;
    DWORD now = GetTickCount();
    if (!next_text || int(now - next_text) >= 0) { // the text (and whether it shows) once a second
        next_text = (now + 1000) | 1;
        bool was = shown;
        shown = badge_text(text, 160);
        if (shown != was)
            logf("overlay: badge %s", shown ? "shown (title screen)" : "hidden");
    }
    if (!shown || !render_text(dev, text))
        return;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    D3DSURFACE_DESC d = {};
    bb->GetDesc(&d);
    IDirect3DStateBlock9 *sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) {
        bb->Release();
        return;
    }
    dev->GetRenderTarget(0, &rt);
    dev->SetRenderTarget(0, bb);
    if (SUCCEEDED(dev->BeginScene())) {
        D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.f, 1.f};
        dev->SetViewport(&vp);
        dev->SetPixelShader(nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetTexture(0, g_tex);
        dev->SetTexture(1, nullptr);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        // drawn at 3/4 of the texture's size at 1080p, other heights in proportion
        float k = d.Height / 1440.f, w = g_text_w * k, h = g_text_h * k, m = d.Height / 54.f;
        float x0 = d.Width - m - w, y0 = m, x1 = x0 + w, y1 = y0 + h;
        float u = float(g_text_w) / kTexW, v = float(g_text_h) / kTexH;
        Vtx q[4] = {{x0 - .5f, y0 - .5f, 0, 1, 0, 0}, {x1 - .5f, y0 - .5f, 0, 1, u, 0}, {x0 - .5f, y1 - .5f, 0, 1, 0, v},
                    {x1 - .5f, y1 - .5f, 0, 1, u, v}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
        dev->EndScene();
    }
    sb->Apply();
    sb->Release();
    if (rt) {
        dev->SetRenderTarget(0, rt);
        rt->Release();
    }
    bb->Release();
}

HRESULT WINAPI hk_present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    draw_badge(dev);
    return g_present_orig(dev, src, dst, wnd, dirty);
}

} // namespace

// Pad thread, at start: the Present address from a throwaway device, hooked.
void overlay_install()
{
    if (early_flag("no_badge"))
        return;
    HMODULE d3d = LoadLibraryA("d3d9.dll");
    using Create_t = IDirect3D9 *(WINAPI *)(UINT);
    auto create = d3d ? reinterpret_cast<Create_t>(GetProcAddress(d3d, "Direct3DCreate9")) : nullptr;
    IDirect3D9 *d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d) {
        logf("overlay: no Direct3D 9: no badge");
        return;
    }
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddda_coop_badge";
    RegisterClassA(&wc);
    HWND w = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = w;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                 &pp, &dev);
    void *present = nullptr;
    if (SUCCEEDED(hr) && dev) {
        present = (*reinterpret_cast<void ***>(dev))[17];
        dev->Release();
    }
    d->Release();
    DestroyWindow(w);
    if (!present) {
        logf("overlay: throwaway device failed (0x%08lX): no badge", hr);
        return;
    }
    MH_STATUS st = MH_CreateHook(present, reinterpret_cast<void *>(&hk_present), reinterpret_cast<void **>(&g_present_orig));
    if (st == MH_OK)
        st = MH_EnableHook(present);
    logf("overlay: hook Present %p: %s", present, MH_StatusToString(st));
}
