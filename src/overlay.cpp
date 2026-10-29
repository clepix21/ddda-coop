// "Mod active" badge (owner's request, 2026-10-29): on the title screen, its main menu and the loads,
// a small banner at the top right says the co-op mod is loaded, this PC's role and how many players
// are in.
//
// Finding the game's Direct3D 9 device (2026-10-29 notes): a throwaway hardware device cannot be made
// from the mod (D3DERR_INVALIDCALL with every setting) and a reference one's methods are other code.
// The game's device has its own copy of the vtable on the heap (a hooking tool copied it), its methods
// in d3d9.dll; d3d9.dll's own objects with 119-slot vtables share theirs (their slot 17 is called from
// inside d3d9.dll with two arguments: a 5-argument hook there crashed the game). So: the objects
// behind the game's globals (up to three pointers away) whose 119 slots are code, most of them in
// d3d9.dll, read only through ReadProcessMemory (the game frees memory meanwhile: reads checked first
// and made after crashed it); each one's EndScene slot gets a pass-through spy (the stack untouched)
// until a call comes from DDDA.exe with that object as `this`: that is the device.
//
// Drawn right before the frame is shown (the device's Present, slot 17, and its swap chain's, slot 3):
// drawn at EndScene, the badge was under what the game drew afterwards. The text is rendered with GDI
// into a 32-bit DIB, copied into a managed texture (it survives a Reset) and drawn as one screen quad;
// the game's states are kept in a state block made and freed around the draw (state blocks must not
// live across a Reset).
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "game.h"
#include "mod.h"

namespace {

constexpr int kTexW = 2048, kTexH = 128;
constexpr int kEndSceneSlot = 42, kPresentSlot = 17, kSwapPresentSlot = 3;
using Present_t = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using SwapPresent_t = HRESULT(WINAPI *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
Present_t g_present_orig = nullptr;
SwapPresent_t g_swap_present_orig = nullptr;
IDirect3DDevice9 *g_tex_dev = nullptr;
IDirect3DTexture9 *g_tex = nullptr;
wchar_t g_tex_text[160] = L"";
int g_text_w = 0, g_text_h = 0; // pixels used in the texture

struct Vtx {
    float x, y, z, rhw, u, v;
};

// What the badge says now (called once a second), or false when it is not shown. Shown out of the
// play areas (the title screen, its main menu, loads: their current area is no aStage, 0 on the
// title, 2026-10-29), and in a play area while the game is paused (the pause menu "Menu pause" and
// its pages, the map): the game's clock (sGameSys, ms into the day) has not moved for two seconds. No
// fixed word in the game's globals told the menu apart (snapshot diffs with the menu shut, open, shut).
bool badge_text(wchar_t *out, size_t cap)
{
    uintptr_t area = game::at<uintptr_t>(game::kArea, 0);
    uintptr_t cur = area ? game::at<uintptr_t>(area, game::kAreaCurrent) : 0;
    const char *cls = cur ? enemies_class_of(cur) : "";
    static int32_t last_ms = -1;
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    int32_t ms = gm ? game::at<int32_t>(gm, game::kGsDayMs) : -1;
    static int still = 0; // seconds the clock stood still: 2 needed (1 showed it during short load stops)
    still = ms >= 0 && ms == last_ms ? still + 1 : 0;
    bool paused = still >= 2;
    last_ms = ms;
    static bool was_paused = false;
    if (strncmp(cls, "aStage", 6) == 0 && paused != was_paused)
        logf("overlay: the game's clock %s", paused ? "stopped (paused)" : "runs again");
    was_paused = paused;
    if (strncmp(cls, "aStage", 6) == 0 && !paused)
        return false;
    NetRole role = net_role();
    int players = 0;
    for (int s = 0; s < 4; ++s) {
        PlayerInfo p;
        players += net_player_info(s, p) && p.present;
    }
    const wchar_t *who = role == NET_HOST ? L"hôte" : L"invité";
    if (role == NET_OFF)
        _snwprintf(out, cap, L"DDDA Coop actif  ·  seul (pas de partie en réseau)");
    else
        _snwprintf(out, cap, L"DDDA Coop actif  ·  %ls%ls  ·  %d joueur%ls", who, net_shared() ? L", monde partagé" : L"",
                   players < 1 ? 1 : players, players > 1 ? L"s" : L"");
    if (net_old_peer()) {
        size_t len = wcslen(out);
        _snwprintf(out + len, cap - len, L"  \u00b7  un joueur a une autre version du mod !");
    }
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
    g_text_w = std::min(int(sz.cx) + pad * 3 + dot, kTexW);
    g_text_h = std::min(int(sz.cy) + pad, kTexH);
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
                    cr = (cr * (255 - t) + 255 * t) / 255;
                    cg = (cg * (255 - t) + 255 * t) / 255;
                    cb = (cb * (255 - t) + 255 * t) / 255;
                    a = a > t ? a : t;
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

// Right before a frame is shown: the badge over the back buffer, in a scene of its own.
void draw_badge(IDirect3DDevice9 *dev)
{
    static DWORD next_text = 0, last_draw = 0;
    static wchar_t text[160] = L"";
    static bool shown = false;
    DWORD now = GetTickCount();
    if (!next_text || int(now - next_text) >= 0) { // the text (and whether it shows) once a second
        next_text = (now + 1000) | 1;
        bool was = shown;
        shown = badge_text(text, 160);
        if (shown != was)
            logf("overlay: badge %s", shown ? "shown" : "hidden (playing)");
    }
    if (!shown || (last_draw && now - last_draw < 5) || !render_text(dev, text)) // once a frame (both Presents)
        return;
    last_draw = now | 1;
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
        // half the texture's size at 1080p (text about 22 px high), other heights in proportion
        float k = d.Height / 2160.f, w = g_text_w * k, h = g_text_h * k, m = d.Height / 54.f;
        float x0 = d.Width - m - w, y0 = m, x1 = x0 + w, y1 = y0 + h;
        float u = float(g_text_w) / kTexW, v = float(g_text_h) / kTexH;
        Vtx q[4] = {{x0 - .5f, y0 - .5f, 0, 1, 0, 0}, {x1 - .5f, y0 - .5f, 0, 1, u, 0}, {x0 - .5f, y1 - .5f, 0, 1, 0, v},
                    {x1 - .5f, y1 - .5f, 0, 1, u, v}};
        HRESULT dr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
        static bool told = false;
        if (!told) {
            told = true;
            logf("overlay: first badge at (%.0f, %.0f)-(%.0f, %.0f) on %ux%u: 0x%08lX", x0, y0, x1, y1, d.Width, d.Height, dr);
        }
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

bool badge_on()
{
    static int on = -1; // tests: test_badge_noop=1 hooks but draws nothing
    if (on < 0)
        on = early_flag("test_badge_noop") ? 0 : 1;
    return on != 0;
}

HRESULT WINAPI hk_present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    static bool told = false;
    if (!told) {
        told = true;
        logf("overlay: the device's Present is called (device %p)", (void *)dev);
    }
    int64_t t0 = perf_now();
    if (badge_on())
        draw_badge(dev);
    perf_add(PERF_BADGE, t0);
    perf_frame();
    return g_present_orig(dev, src, dst, wnd, dirty);
}

HRESULT WINAPI hk_swap_present(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty, DWORD flags)
{
    static bool told = false;
    IDirect3DDevice9 *dev = nullptr;
    if (sc && SUCCEEDED(sc->GetDevice(&dev)) && dev) {
        if (!told) {
            told = true;
            logf("overlay: the swap chain's Present is called (device %p)", (void *)dev);
        }
        int64_t t0 = perf_now();
        if (badge_on())
            draw_badge(dev);
        perf_add(PERF_BADGE, t0);
        if (!g_present_orig) // the device's own Present counts the frames when it is taken
            perf_frame();
        dev->Release();
    }
    return g_swap_present_orig(sc, src, dst, wnd, dirty, flags);
}

} // namespace

// The EndScene spy (see the top): passes every call on, notes the first one on the watched object.
extern "C" void *g_spy_orig asm("g_spy_orig");
void *g_spy_orig = nullptr; // read by end_scene_spy
namespace {
volatile LONG g_spy_verdict = 0; // 0 waiting, 1 the game's device
uintptr_t g_spy_this = 0;
extern "C" void __cdecl spy_check(const uint32_t *sp) asm("spy_check");
extern "C" void __cdecl spy_check(const uint32_t *sp)
{
    // the vtable may be shared: calls on other objects of that class pass; a call from the game on the
    // watched object is the device
    if (g_spy_verdict || sp[1] != g_spy_this || sp[0] < 0x00401000 || sp[0] >= 0x01A0C000)
        return;
    logf("overlay: EndScene called from %08X on %08X: the game's device", sp[0], sp[1]);
    InterlockedExchange(&g_spy_verdict, 1);
}
extern "C" __attribute__((naked)) void end_scene_spy() asm("end_scene_spy");
extern "C" __attribute__((naked)) void end_scene_spy()
{
    asm volatile("pushal\n\t"
                 "leal 32(%esp), %eax\n\t"
                 "pushl %eax\n\t"
                 "call spy_check\n\t"
                 "addl $4, %esp\n\t"
                 "popal\n\t"
                 "jmp *g_spy_orig\n\t");
}

struct Range {
    uintptr_t base;
    size_t size;
    bool has(uintptr_t p) const { return p >= base && p < base + size; }
};
Range module_range(HMODULE m)
{
    if (!m)
        return {0, 0};
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(m) + dos->e_lfanew);
    return {reinterpret_cast<uintptr_t>(m), nt->OptionalHeader.SizeOfImage};
}

bool code_address(uintptr_t p)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    return VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof mbi) && mbi.Type == MEM_IMAGE &&
           (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE | PAGE_EXECUTE_WRITECOPY));
}

// A read that fails instead of faulting.
bool peek(uintptr_t at, void *out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, n, &got) && got == n;
}

// Device candidates (see the top), the ones with a vtable of their own (outside d3d9.dll) first.
int device_candidates(uintptr_t *out, int cap)
{
    Range d3d = module_range(GetModuleHandleA("d3d9.dll"));
    if (!d3d.base)
        return 0;
    constexpr uintptr_t kLo = 0x01621000, kHi = 0x019AB1E0;
    std::vector<uint32_t> globals((kHi - kLo) / 4);
    if (!peek(kLo, globals.data(), globals.size() * 4))
        return 0;
    std::unordered_set<uint32_t> seen;
    int n = 0;
    auto consider = [&](uint32_t obj) {
        if (n >= cap || obj < 0x10000 || obj >= 0x7FFF0000 || (obj & 3) || !seen.insert(obj).second)
            return;
        uint32_t vt = 0;
        if (!peek(obj, &vt, 4) || vt < 0x10000 || (vt & 3))
            return;
        uint32_t slots[119];
        if (!peek(vt, slots, sizeof slots))
            return;
        int inside = 0;
        for (uint32_t f : slots) {
            if (d3d.has(f))
                ++inside;
            else if (!code_address(f))
                return;
        }
        if (inside >= 100)
            out[n++] = obj;
    };
    std::unordered_set<uint32_t> holders;
    uint32_t fields[0x100];
    for (uint32_t obj : globals) {
        if (n >= cap)
            break;
        if (obj < 0x10000 || obj >= 0x7FFF0000 || (obj & 3) || !holders.insert(obj).second)
            continue;
        consider(obj);
        if (peek(obj, fields, sizeof fields))
            for (uint32_t f : fields)
                consider(f);
    }
    for (int depth = 0, from = 0; depth < 2; ++depth) { // what the d3d9.dll objects found point to
        int upto = n;
        for (int c = from; c < upto && n < cap; ++c)
            if (peek(out[c], fields, sizeof fields))
                for (uint32_t f : fields)
                    consider(f);
        from = upto;
    }
    std::stable_sort(out, out + n, [&](uintptr_t x, uintptr_t y) {
        uint32_t vx = 0, vy = 0; // through peek: a candidate may be freed meanwhile (a direct read here
        peek(x, &vx, 4);         // or in install_thread closed a game copy at start, quest suite 2026-10-29)
        peek(y, &vy, 4);
        return !d3d.has(vx) && d3d.has(vy);
    });
    return n;
}

bool set_slot(void **slot, void *to)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old))
        return false;
    InterlockedExchange(reinterpret_cast<volatile LONG *>(slot), LONG(reinterpret_cast<uintptr_t>(to)));
    VirtualProtect(slot, 4, old, &old);
    return true;
}

// Both Presents of the game's device taken (the device's own vtable copy; the swap chain's vtable).
void take_presents(IDirect3DDevice9 *dev)
{
    void **slot = *reinterpret_cast<void ***>(dev) + kPresentSlot;
    g_present_orig = reinterpret_cast<Present_t>(*slot);
    bool a = set_slot(slot, reinterpret_cast<void *>(&hk_present));
    IDirect3DSwapChain9 *sc = nullptr;
    bool b = false;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        void **sslot = *reinterpret_cast<void ***>(sc) + kSwapPresentSlot;
        g_swap_present_orig = reinterpret_cast<SwapPresent_t>(*sslot);
        b = set_slot(sslot, reinterpret_cast<void *>(&hk_swap_present));
        sc->Release();
    }
    logf("overlay: device %p: its Present %s, its swap chain's %s", (void *)dev, a ? "taken" : "not taken", b ? "taken" : "not taken");
}

DWORD WINAPI install_thread(LPVOID)
{
    std::unordered_set<uintptr_t> rejected; // vtables watched without a call from the game
    for (int round = 0; round < 60; ++round) { // the device comes with the game's window: up to 2 minutes
        Sleep(2000);
        uintptr_t cand[16];
        int n = device_candidates(cand, 16);
        for (int c = 0; c < n; ++c) {
            uint32_t vt = 0;
            if (!peek(cand[c], &vt, 4) || vt < 0x10000)
                continue;
            if (rejected.count(vt))
                continue;
            void **slot = reinterpret_cast<void **>(uintptr_t(vt)) + kEndSceneSlot;
            void *orig = nullptr;
            if (!peek(uintptr_t(slot), &orig, 4) || !orig)
                continue;
            g_spy_orig = orig;
            g_spy_this = cand[c];
            InterlockedExchange(&g_spy_verdict, 0);
            if (!set_slot(slot, reinterpret_cast<void *>(&end_scene_spy)))
                continue;
            for (int w = 0; w < 40 && !g_spy_verdict; ++w)
                Sleep(50);
            set_slot(slot, orig);
            if (g_spy_verdict) {
                take_presents(reinterpret_cast<IDirect3DDevice9 *>(cand[c]));
                return 0;
            }
            rejected.insert(vt);
        }
    }
    logf("overlay: the game's device was not found: no badge");
    return 0;
}

} // namespace

// Pad thread, at start: finds the game's device on a thread of its own.
void overlay_install()
{
    if (early_flag("no_badge"))
        return;
    if (HANDLE t = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}
