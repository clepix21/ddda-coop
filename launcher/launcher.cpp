// DDDA Coop: a small window to run beside the game. It finds the Steam install of Dragon's Dogma:
// Dark Arisen, checks the DDDA.exe build, copies the player's saves to Documents, installs
// dinput8.dll and writes coop.ini for the chosen role, starts the game through Steam, and shows
// the co-op state read from the game's coop.log, and the players from coop_status.txt (written by the
// mod every second in the shared world). "Retirer le mod" deletes dinput8.dll and coop.ini.
//
// Files next to DDDA-Coop.exe: dinput8.dll (the mod), coop.ini (template, optional),
// DDDA-Coop.ini (the launcher's own choices). Tests: --game <dir> and --steam <dir> replace the
// folders found in the registry, --docs <dir> replaces Documents, --no-launch skips starting Steam.
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../src/version.h"
#include "theme.h"

namespace {

const wchar_t kTitle[] = L"DDDA Coop";
const char kSupportedSha[] = "19FACC2642F79A09D14CE55EACE5F228161F2F8BAB60EC67EB3C6196CC6277B8";
const char kOurMarker[] = "ddda-pawn-coop loaded"; // a string only our dinput8.dll holds

enum {
    ID_GAME_EDIT = 100, ID_BROWSE, ID_HOST, ID_GUEST, ID_OWN_CHAR, ID_DEBUG, ID_LAUNCH, ID_REMOVE,
    ID_BACKUP, ID_EXPORT, ID_EVENTS, ID_SOLO, ID_KEEP, ID_TIMER, ID_PLAYERS, ID_HELP, ID_EVENTS_TOGGLE, ID_FIRST_HELP,
    ID_OPEN_BACKUPS, ID_COPY_CODE, ID_CODE_EDIT, ID_CODE_CLEAR, ID_BACKUP_LIST, ID_UPDATE, ID_CHECK_UPDATES, ID_PROJECT,
    ID_NAV = 200 // ID_NAV + page: the sidebar's buttons
};
// the screens, in the sidebar's order
enum Page { PAGE_PLAY, PAGE_SALON, PAGE_SAVES, PAGE_SETTINGS, PAGE_LOG, PAGE_COUNT };
const wchar_t *const kPageNames[PAGE_COUNT] = {L"Jouer", L"Salon", L"Sauvegardes", L"Paramètres", L"Journal"};
// Segoe MDL2 Assets glyphs (Windows 10 and 11): play, people, save, settings, list
const wchar_t *const kPageIcons[PAGE_COUNT] = {L"\uE768", L"\uE716", L"\uE74E", L"\uE713", L"\uE8FD"};
int g_page = PAGE_PLAY;
uint64_t g_self_steam = 0; // this player's Steam id, from the mod (the host's code)
HWND g_code_edit = nullptr, g_copy_code = nullptr, g_code_clear = nullptr, g_backup_list = nullptr;
std::vector<std::wstring> g_backup_dirs; // the backup list's folders, newest first

// The host's code: its Steam account number (the low 32 bits of its Steam id) in Crockford's base 32,
// 7 characters shown "ABC-DEFG". The guest's launcher turns it back into peer_steamid= for the mod.
const char *const kCode = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
std::wstring code_from_steam(uint64_t id)
{
    uint32_t acct = uint32_t(id & 0xFFFFFFFFu);
    wchar_t c[9];
    for (int k = 6; k >= 0; --k) {
        c[k < 3 ? k : k + 1] = wchar_t(kCode[acct & 31]);
        acct >>= 5;
    }
    c[3] = L'-';
    c[8] = 0;
    return c;
}
bool steam_from_code(const std::wstring &text, uint64_t &id)
{
    uint64_t v = 0;
    int n = 0;
    for (wchar_t ch : text) {
        if (ch == L'-' || ch == L' ')
            continue;
        wchar_t u = wchar_t(towupper(ch));
        if (u == L'O')
            u = L'0';
        if (u == L'I' || u == L'L')
            u = L'1';
        const char *at = u < 128 ? strchr(kCode, char(u)) : nullptr;
        if (!at || !*at)
            return false;
        v = v * 32 + uint64_t(at - kCode);
        ++n;
    }
    if (n != 7 || v > 0xFFFFFFFFull || v == 0)
        return false;
    id = 76561197960265728ull + v;
    return true;
}
enum Role { ROLE_HOST, ROLE_GUEST, ROLE_SOLO };
int g_role = ROLE_HOST;
// the switches (own character, keep progress, detailed log): on/off and their label without the state
struct Switch {
    HWND h;
    bool on;
    std::wstring label;
};
std::vector<Switch> g_switches;
int g_game_state = 0;     // 0 DDDA.exe not found, 1 found but another version, 2 supported (the header's line)
bool g_log_unseen = false; // a warning or an error in the journal since it was last shown (a dot on its icon)

HWND g_wnd, g_game_edit, g_version, g_host, g_guest, g_solo, g_own_char, g_keep, g_debug, g_launch, g_state, g_events,
    g_players, g_backup_note, g_update, g_check_updates;
HFONT g_font, g_bold, g_head, g_title, g_icon, g_small, g_small_icon, g_code_font;
HBRUSH g_bg_brush, g_card_brush;
int g_dpi = 96;
RECT g_state_rect = {}, g_header_rect = {};

// colours: the theme's tokens (theme.h); the state also in words and a symbol, not only in colour
const COLORREF kBg = theme::kBg, kCard = theme::kSurface, kText = theme::kText, kMuted = theme::kMuted,
               kOk = theme::kOk, kWarn = theme::kWarn, kBad = theme::kBad, kInfo = theme::kInfo;
int px(int v) { return MulDiv(v, g_dpi, 96); }
std::wstring g_exe_dir, g_game_dir, g_steam_dir, g_docs_dir;
bool g_no_launch = false;
std::wstring g_test_update; // --test-update <tag>
bool g_supported = false;
COLORREF g_state_color = kMuted;
FILETIME g_launch_time = {};
long long g_log_pos = 0;
bool g_watching = false;
int g_friends = 0;
bool g_world_received = false;
// for the clear error messages: what was seen since the launch
DWORD g_launch_tick = 0, g_steam_wait_since = 0;
bool g_mod_loaded = false, g_link_up = false, g_host_found = false, g_game_seen = false, g_told_no_mod = false,
     g_told_steam = false, g_told_no_host = false, g_told_closed = false;

// ---- small helpers ----

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring &p, std::string &out, long long from = 0)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {}, pos = {};
    GetFileSizeEx(h, &size);
    out.clear();
    if (size.QuadPart > from) {
        pos.QuadPart = from;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.resize(size_t(size.QuadPart - from));
        DWORD got = 0;
        ReadFile(h, &out[0], DWORD(out.size()), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(h);
    return true;
}

bool write_file(const std::wstring &p, const std::string &data)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

std::string sha256_file(const std::wstring &p)
{
    std::string data;
    if (!read_file(p, data))
        return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(&data[0]), ULONG(data.size()), 0);
        BCryptFinishHash(hash, digest, sizeof digest, 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    char b[3];
    for (unsigned char c : digest) {
        snprintf(b, sizeof b, "%02X", c);
        hex += b;
    }
    return hex;
}

bool game_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe = {sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"DDDA.exe") == 0;
    CloseHandle(snap);
    return found;
}

std::wstring window_text(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    if (n)
        GetWindowTextW(h, &s[0], n + 1);
    return s;
}

int ask(const wchar_t *text, UINT flags) { return MessageBoxW(g_wnd, text, kTitle, flags); }

// ---- Steam and game folders ----

std::wstring steam_dir()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size))
        return {};
    std::wstring s = buf;
    for (auto &c : s)
        if (c == L'/')
            c = L'\\';
    return s;
}

// Every value of "key" in a Valve .vdf/.acf text ("key"  "value"), with \\ unescaped.
std::vector<std::string> vdf_values(const std::string &text, const char *key)
{
    std::vector<std::string> out;
    std::string k = std::string("\"") + key + "\"";
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + 1)) {
        size_t q1 = text.find('"', at + k.size());
        if (q1 == std::string::npos)
            break;
        std::string v;
        size_t i = q1 + 1;
        for (; i < text.size() && text[i] != '"' && text[i] != '\n'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size())
                ++i;
            v += text[i];
        }
        out.push_back(v);
    }
    return out;
}

std::wstring find_game(const std::wstring &steam)
{
    if (steam.empty())
        return {};
    std::vector<std::wstring> libs = {steam};
    std::string vdf;
    if (read_file(steam + L"\\steamapps\\libraryfolders.vdf", vdf))
        for (auto &p : vdf_values(vdf, "path"))
            libs.push_back(widen(p));
    for (auto &lib : libs) {
        std::wstring dir = L"DDDA";
        std::string acf;
        if (read_file(lib + L"\\steamapps\\appmanifest_367500.acf", acf)) {
            auto v = vdf_values(acf, "installdir");
            if (!v.empty())
                dir = widen(v[0]);
        }
        std::wstring game = lib + L"\\steamapps\\common\\" + dir;
        if (exists(game + L"\\DDDA.exe"))
            return game;
    }
    return {};
}

// ---- the launcher's own settings (DDDA-Coop.ini next to the exe) ----

std::wstring settings_path() { return g_exe_dir + L"\\DDDA-Coop.ini"; }

std::wstring setting(const wchar_t *key, const wchar_t *def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"launcher", key, def, buf, 1024, settings_path().c_str());
    return buf;
}

bool switch_on(HWND h)
{
    for (const Switch &w : g_switches)
        if (w.h == h)
            return w.on;
    return false;
}

void save_settings()
{
    auto p = settings_path();
    WritePrivateProfileStringW(L"launcher", L"game", g_game_dir.c_str(), p.c_str());
    WritePrivateProfileStringW(L"launcher", L"role", g_role == ROLE_GUEST ? L"guest" : g_role == ROLE_SOLO ? L"solo" : L"host",
                               p.c_str());
    WritePrivateProfileStringW(L"launcher", L"keep_progress", switch_on(g_keep) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"own_character", switch_on(g_own_char) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"debug_log", switch_on(g_debug) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"check_updates", switch_on(g_check_updates) ? L"1" : L"0", p.c_str());
}

// ---- the update check: the project's newest release on GitHub (a release from the public repo) ----

const wchar_t *const kRepoUrl = L"https://github.com/clepix21/ddda-coop";
const UINT WM_UPDATE_FOUND = WM_APP + 7; // the check found a newer version: g_update_tag
std::wstring g_update_tag;               // "v0.9.2", set once by the check's thread before it posts

// "v0.9.2" or "0.9.2" -> {0, 9, 2}; false when it is not that shape
bool parse_version(const std::string &t, int out[3])
{
    size_t i = t.size() && (t[0] == 'v' || t[0] == 'V') ? 1 : 0;
    for (int k = 0; k < 3; ++k) {
        if (i >= t.size() || !isdigit(static_cast<unsigned char>(t[i])))
            return false;
        out[k] = 0;
        while (i < t.size() && isdigit(static_cast<unsigned char>(t[i])))
            out[k] = out[k] * 10 + (t[i++] - '0');
        if (k < 2 && (i >= t.size() || t[i++] != '.'))
            return false;
    }
    return true;
}

// The newest release, drafts aside (prereleases count: the mod is an alpha). Quiet on every failure
// (no network, the repo still private: 404); the launcher works the same without it.
DWORD WINAPI check_updates(void *)
{
    std::string body;
    if (!g_test_update.empty())
        body = "[{\"tag_name\": \"" + std::string(g_test_update.begin(), g_test_update.end()) + "\"}]";
    else {
    HINTERNET s = WinHttpOpen((L"DDDA-Coop/" + widen(DDDA_COOP_VERSION)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/repos/clepix21/ddda-coop/releases?per_page=1", nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    DWORD status = 0, size = sizeof status;
    if (r) {
        WinHttpSetTimeouts(r, 5000, 5000, 5000, 5000);
        if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            char buf[4096];
            DWORD got = 0;
            while (body.size() < 256 * 1024 && WinHttpReadData(r, buf, sizeof buf, &got) && got)
                body.append(buf, got);
        }
    }
    for (HINTERNET x : {r, c, s})
        if (x)
            WinHttpCloseHandle(x);
    }
    size_t k = body.find("\"tag_name\"");
    size_t q1 = k == std::string::npos ? k : body.find('"', body.find(':', k) + 1);
    size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 > 32)
        return 0;
    std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
    int have[3], got[3];
    if (!parse_version(DDDA_COOP_VERSION, have) || !parse_version(tag, got))
        return 0;
    if (std::lexicographical_compare(have, have + 3, got, got + 3)) {
        g_update_tag = widen(tag);
        PostMessageW(g_wnd, WM_UPDATE_FOUND, 0, 0);
    }
    return 0;
}

// ---- state shown in the window ----

HBRUSH g_state_brush = nullptr;

void set_state(const std::wstring &text, COLORREF color)
{
    g_state_color = color;
    if (g_state_brush)
        DeleteObject(g_state_brush);
    g_state_brush = CreateSolidBrush(theme::mix(theme::kSurface, color, 0.08f)); // the state card's tint (paint)
    // the meaning is in the words and the symbol too, not only in the colour
    const wchar_t *sym = color == kOk ? L"\u2714  " : color == kBad ? L"\u2716  " : color == kWarn ? L"\u26A0  " : L"\u25CF  ";
    SetWindowTextW(g_state, (sym + text).c_str());
    InvalidateRect(g_state, nullptr, TRUE);
    InvalidateRect(g_wnd, &g_state_rect, TRUE);
}

// The kind of an event, from its words: 0 information, 1 good news (connected, received, copied),
// 2 a warning (disconnected, lost, waiting), 3 an error. Its icon and colour in the list.
int event_kind(const std::wstring &t)
{
    auto has = [&](const wchar_t *w) { return t.find(w) != std::wstring::npos; };
    if (has(L"Impossible") || has(L"non prise en charge") || has(L"autre version") || has(L"planté") || has(L"pas chargé") ||
        has(L"ne démarre pas") || has(L"ne répond pas") || has(L"échoué"))
        return 3;
    if (has(L"déconnecté") || has(L"perdue") || has(L"pas maintenant") || has(L"en attente") || has(L"Aucun") ||
        has(L"introuvable") || has(L"s'est fermé"))
        return 2;
    if (has(L"connecté") || has(L"Connecté") || has(L"trouvé") || has(L"reçu") || has(L"copiées") || has(L"installé") ||
        has(L"est arrivé") || has(L"De retour") || has(L"revenu") || has(L"ajoutée") || has(L"enregistrée") || has(L"copié"))
        return 1;
    return 0;
}

void layout_events();

void add_event(const std::wstring &text)
{
    wchar_t stamp[16];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 16, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    int i = int(SendMessageW(g_events, LB_ADDSTRING, 0, LPARAM((stamp + text).c_str())));
    SendMessageW(g_events, LB_SETITEMDATA, i, LPARAM(event_kind(text)));
    SendMessageW(g_events, LB_SETTOPINDEX, i, 0);
    if (g_page == PAGE_LOG) {
        ShowWindow(g_events, SW_SHOW);
        InvalidateRect(g_wnd, nullptr, FALSE); // the counts
    }
    if (event_kind(text) >= 2 && g_page != PAGE_LOG) { // a warning or an error: a dot on the journal's icon
        g_log_unseen = true;
        if (HWND nav = GetDlgItem(g_wnd, ID_NAV + PAGE_LOG))
            InvalidateRect(nav, nullptr, FALSE);
    }
    layout_events();
}

// A backup copy's row: a save icon, the date, the newest marked.
void draw_backup(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == UINT(-1))
        return;
    bool sel = d->itemState & ODS_SELECTED;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, g_card_brush);
    RECT row = r;
    InflateRect(&row, -px(4), -px(2));
    if (sel)
        theme::round_rect(d->hDC, row, px(theme::kRadiusSmall), theme::kSurfaceHover, theme::kAccent);
    wchar_t text[128] = L"";
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, LPARAM(text));
    SetBkMode(d->hDC, TRANSPARENT);
    RECT t = row;
    t.left += px(12);
    SetTextColor(d->hDC, kMuted);
    HGDIOBJ old = SelectObject(d->hDC, g_small_icon);
    DrawTextW(d->hDC, L"\uE74E", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(d->hDC, g_font);
    t.left += px(26);
    SetTextColor(d->hDC, kText);
    DrawTextW(d->hDC, text, -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (d->itemID == 0) {
        SetTextColor(d->hDC, kMuted);
        RECT n = row;
        n.right -= px(12);
        DrawTextW(d->hDC, L"la plus récente", -1, &n, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(d->hDC, old);
}

void draw_event(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == UINT(-1))
        return;
    static const COLORREF colors[4] = {kInfo, kOk, kWarn, kBad};
    static const wchar_t *icons[4] = {L"\u25CF", L"\u2714", L"\u26A0", L"\u2716"};
    int kind = int(d->itemData) & 3;
    wchar_t text[600] = L"";
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, LPARAM(text));
    FillRect(d->hDC, &d->rcItem, g_card_brush);
    SetBkMode(d->hDC, TRANSPARENT);
    RECT r = d->rcItem;
    r.left += px(4);
    // "hh:mm:ss  " then the icon in the kind's colour, then the words
    std::wstring all = text;
    std::wstring stamp = all.substr(0, all.size() >= 10 ? 10 : all.size()), rest = all.size() > 10 ? all.substr(10) : L"";
    SetTextColor(d->hDC, kMuted);
    DrawTextW(d->hDC, stamp.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SIZE sz = {};
    GetTextExtentPoint32W(d->hDC, stamp.c_str(), int(stamp.size()), &sz);
    r.left += sz.cx;
    SetTextColor(d->hDC, colors[kind]);
    DrawTextW(d->hDC, icons[kind], -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    r.left += px(18);
    SetTextColor(d->hDC, kind == 3 ? kBad : kind == 2 ? kWarn : kText);
    DrawTextW(d->hDC, rest.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

bool guest_role() { return g_role == ROLE_GUEST; }
bool solo_role() { return g_role == ROLE_SOLO; }

void update_controls()
{
    // the salon's connection block: the host copies its code, the guest pastes it
    if (g_copy_code) {
        bool salon = g_page == PAGE_SALON;
        ShowWindow(g_copy_code, salon && g_role == ROLE_HOST ? SW_SHOW : SW_HIDE);
        EnableWindow(g_copy_code, g_self_steam != 0);
        ShowWindow(g_code_edit, salon && g_role == ROLE_GUEST ? SW_SHOW : SW_HIDE);
        ShowWindow(g_code_clear, salon && g_role == ROLE_GUEST ? SW_SHOW : SW_HIDE);
    }
    // the guest's own options only under the guest's card
    ShowWindow(g_own_char, g_page == PAGE_PLAY && guest_role() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_keep, g_page == PAGE_PLAY && guest_role() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_update, g_page == PAGE_PLAY && !g_update_tag.empty() ? SW_SHOW : SW_HIDE);
    EnableWindow(g_launch, g_supported);
}

void check_game()
{
    g_game_dir = window_text(g_game_edit);
    g_supported = false;
    g_game_state = 0;
    if (g_game_dir.empty() || !exists(g_game_dir + L"\\DDDA.exe")) {
        SetWindowTextW(g_version, L"DDDA.exe introuvable dans ce dossier.");
    } else {
        SetWindowTextW(g_version, L"Vérification de DDDA.exe…");
        UpdateWindow(g_version);
        g_supported = sha256_file(g_game_dir + L"\\DDDA.exe") == kSupportedSha;
        SetWindowTextW(g_version, g_supported ? L"Version du jeu prise en charge."
                                              : L"Version de DDDA.exe non prise en charge : le mod ne marcherait pas.");
        g_game_state = g_supported ? 2 : 1;
    }
    update_controls();
    if (g_wnd)
        InvalidateRect(g_wnd, nullptr, FALSE);
}

// ---- actions ----

// Copies every file under from into to (folders included). False on the first failure.
bool copy_tree(const std::wstring &from, const std::wstring &to)
{
    if (SHCreateDirectoryExW(nullptr, to.c_str(), nullptr) != ERROR_SUCCESS && !is_dir(to))
        return false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return true;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = copy_tree(from + L"\\" + n, to + L"\\" + n) && ok;
        else
            ok = CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), TRUE) && ok;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

std::wstring documents_dir()
{
    if (!g_docs_dir.empty())
        return g_docs_dir;
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)))
        s = p;
    CoTaskMemFree(p);
    return s;
}

// Copies Steam\userdata\<id>\367500\remote of every account on this PC to
// Documents\DDDA Coop\Sauvegardes\<date>\<id>. Returns the folder, or empty on failure.
std::wstring backup_saves(int *accounts)
{
    *accounts = 0;
    if (g_steam_dir.empty())
        return {};
    wchar_t stamp[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 32, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring dest = documents_dir() + L"\\DDDA Coop\\Sauvegardes\\" + stamp;
    for (int n = 2; exists(dest); ++n) // two copies in the same second
        dest = documents_dir() + L"\\DDDA Coop\\Sauvegardes\\" + stamp + L"_" + std::to_wstring(n);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((g_steam_dir + L"\\userdata\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    bool ok = true;
    do {
        std::wstring id = fd.cFileName;
        std::wstring remote = g_steam_dir + L"\\userdata\\" + id + L"\\367500\\remote";
        if (id == L"." || id == L".." || !is_dir(remote))
            continue;
        ok = copy_tree(remote, dest + L"\\" + id) && ok;
        ++*accounts;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok && *accounts ? dest : std::wstring();
}

bool our_dll(const std::wstring &p)
{
    std::string data;
    return read_file(p, data) && data.find(kOurMarker) != std::string::npos;
}

// coop.ini for the game: the existing one (or the template next to the launcher) without the lines
// the launcher sets, then those lines.
bool write_coop_ini()
{
    std::string base;
    if (!read_file(g_game_dir + L"\\coop.ini", base) && !read_file(g_exe_dir + L"\\coop.ini", base))
        base = "p2_pad=1\nidle_pad=3\nauto_possess=0\nblock_saves=0\nguest_cam_pawn=1\n";
    const char *ours[] = {"role=", "#role=", "transport=", "debug_log=", "send_character=", "keep_progress=", "peer_steamid=",
                          "# Lines below are set by DDDA-Coop.exe"};
    std::string out, line;
    for (size_t i = 0; i <= base.size(); ++i) {
        if (i < base.size() && base[i] != '\n') {
            line += base[i];
            continue;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        bool drop = false;
        for (const char *k : ours)
            drop = drop || line.compare(0, strlen(k), k) == 0;
        if (!drop && !(line.empty() && i == base.size()))
            out += line + "\n";
        line.clear();
    }
    bool guest = guest_role();
    out += "# Lines below are set by DDDA-Coop.exe\n";
    if (!solo_role()) { // solo: the mod only adds the co-op progress kept for this PC's character
        out += guest ? "role=guest\n" : "role=host\n";
        out += "transport=steam\n";
    }
    out += switch_on(g_keep) ? "keep_progress=1\n" : "keep_progress=0\n";
    out += switch_on(g_debug) ? "debug_log=1\n" : "debug_log=0\n";
    if (guest && !switch_on(g_own_char))
        out += "send_character=0\n";
    uint64_t host_id = 0;
    if (guest && g_code_edit && steam_from_code(window_text(g_code_edit), host_id)) // the salon's code: this host only
        out += "peer_steamid=" + std::to_string(host_id) + "\n";
    return write_file(g_game_dir + L"\\coop.ini", out);
}

bool install_dll()
{
    std::wstring src = g_exe_dir + L"\\dinput8.dll", dst = g_game_dir + L"\\dinput8.dll";
    if (!exists(src)) {
        ask(L"dinput8.dll manque à côté de DDDA-Coop.exe : garde les deux fichiers dans le même dossier.", MB_ICONERROR);
        return false;
    }
    if (exists(dst) && !our_dll(dst)) {
        ask(L"Le dossier du jeu contient déjà un autre dinput8.dll (un autre mod ?).\n\n"
            L"Déplace-le ailleurs toi-même, puis réessaie. DDDA Coop ne le remplace pas.",
            MB_ICONWARNING);
        return false;
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        ask(L"Impossible de copier dinput8.dll dans le dossier du jeu.", MB_ICONERROR);
        return false;
    }
    return true;
}

// "Last backup copy: when, where" under the buttons, kept in DDDA-Coop.ini.
void show_backup_note()
{
    std::wstring when = setting(L"last_backup_time", L""), where = setting(L"last_backup", L"");
    std::wstring text = when.empty() ? L"Copie de secours automatique de tes sauvegardes à chaque lancement (aucune encore)."
                                     : L"Dernière copie de secours : " + when + L" \u2192 " + where;
    if (g_backup_note)
        SetWindowTextW(g_backup_note, text.c_str());
}

// The backup copies (Documents\\DDDA Coop\\Sauvegardes\\<date>), newest first, in the Sauvegardes screen.
void refresh_backups()
{
    if (!g_backup_list)
        return;
    std::wstring root = documents_dir() + L"\\DDDA Coop\\Sauvegardes";
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                names.push_back(fd.cFileName);
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(names.rbegin(), names.rend()); // "2026-10-29_22-07-26": the name sorts by date
    g_backup_dirs.clear();
    SendMessageW(g_backup_list, LB_RESETCONTENT, 0, 0);
    for (const std::wstring &n : names) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        std::wstring shown = n;
        if (swscanf(n.c_str(), L"%d-%d-%d_%d-%d-%d", &y, &mo, &d, &h, &mi, &se) == 6) {
            wchar_t t[64];
            swprintf(t, 64, L"%02d/%02d/%04d  %02d:%02d:%02d", d, mo, y, h, mi, se);
            shown = t;
        }
        g_backup_dirs.push_back(root + L"\\" + n);
        SendMessageW(g_backup_list, LB_ADDSTRING, 0, LPARAM(shown.c_str()));
    }
    if (!names.empty())
        SendMessageW(g_backup_list, LB_SETCURSEL, 0, 0);
}

void note_backup(const std::wstring &where)
{
    wchar_t when[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(when, 32, L"%02d/%02d %02d:%02d", t.wDay, t.wMonth, t.wHour, t.wMinute);
    WritePrivateProfileStringW(L"launcher", L"last_backup_time", when, settings_path().c_str());
    WritePrivateProfileStringW(L"launcher", L"last_backup", where.c_str(), settings_path().c_str());
    show_backup_note();
    refresh_backups();
}

// The game's controls and the launcher's own, in a message box (screen readers read it); shown once by
// itself at the first start.
void show_help()
{
    MessageBoxW(g_wnd,
                L"Dans le jeu\n"
                L"  \u2022 Invité : attends « Connecté ! » dans cette fenêtre avant d'appuyer sur Start.\n"
                L"  \u2022 Explorer seul / revenir : LB + RB + Y maintenus 1,5 s (ou F7).\n"
                L"  \u2022 Relever un joueur à terre : maintiens B à côté de lui (3 s).\n"
                L"  \u2022 Saisir un joueur ou un petit monstre : RT.\n"
                L"  \u2022 Le bandeau « DDDA Coop actif » (écran titre, menu pause, carte) dit que le mod tourne.\n\n"
                L"Ici\n"
                L"  \u2022 Une copie de secours de tes sauvegardes est faite à chaque lancement ;\n"
                L"    « Copier mes sauvegardes » en fait une tout de suite et ouvre le dossier.\n"
                L"  \u2022 En cas de souci : « Exporter le journal » et envoie le fichier.\n"
                L"  \u2022 Clavier : Alt + la lettre soulignée, Tab pour passer d'un élément à l'autre.",
                L"DDDA Coop \u2013 Aide", MB_ICONINFORMATION | MB_OK);
}

void on_launch()
{
    check_game();
    if (!g_supported)
        return;
    if (game_running()) {
        ask(L"Le jeu est déjà lancé. Quitte-le d'abord : les réglages ne s'appliquent qu'au démarrage.", MB_ICONINFORMATION);
        return;
    }
    int accounts = 0;
    std::wstring backup = backup_saves(&accounts);
    if (backup.empty() &&
        ask(L"La copie de secours de tes sauvegardes n'a pas marché (aucune trouvée, ou erreur).\n\nLancer quand même ?",
            MB_ICONWARNING | MB_YESNO) != IDYES)
        return;
    if (!backup.empty()) {
        add_event(L"Sauvegardes copiées dans " + backup);
        note_backup(backup);
    }
    if (!install_dll())
        return;
    if (!write_coop_ini()) {
        ask(L"Impossible d'écrire coop.ini dans le dossier du jeu.", MB_ICONERROR);
        return;
    }
    save_settings();
    bool guest = guest_role();
    add_event(guest ? L"Mod installé en invité." : solo_role() ? L"Mod installé pour jouer seul." : L"Mod installé en hôte.");
    if (solo_role() && exists(g_game_dir + L"\\coop_progress\\pending.txt"))
        add_event(L"Ta progression co-op sera ajoutée à ta partie quand tu la chargeras.");

    GetSystemTimeAsFileTime(&g_launch_time);
    g_launch_tick = GetTickCount();
    g_steam_wait_since = 0;
    g_mod_loaded = g_link_up = g_host_found = g_game_seen = false;
    g_told_no_mod = g_told_steam = g_told_no_host = g_told_closed = false;
    g_log_pos = 0;
    g_watching = true;
    g_friends = 0;
    g_world_received = false;
    if (!g_no_launch)
        ShellExecuteW(g_wnd, L"open", L"steam://rungameid/367500", nullptr, nullptr, SW_SHOWNORMAL);
    add_event(L"Lancement du jeu par Steam…");
    set_state(guest ? L"Lancement… Reste sur l'écran titre jusqu'à ce que l'hôte soit trouvé."
                    : L"Lancement… Charge ta partie normalement.",
              kMuted);
}

void on_remove()
{
    check_game();
    if (g_game_dir.empty())
        return;
    if (game_running()) {
        ask(L"Quitte le jeu d'abord.", MB_ICONINFORMATION);
        return;
    }
    std::wstring dll = g_game_dir + L"\\dinput8.dll", ini = g_game_dir + L"\\coop.ini";
    if (!exists(dll) && !exists(ini)) {
        ask(L"Le mod n'est pas installé dans ce dossier.", MB_ICONINFORMATION);
        return;
    }
    // always confirmed, "Non" by default (owner's direction, 2026-10-29)
    if (ask((L"Retirer DDDA Coop de ce dossier ?\n\n" + g_game_dir +
                L"\n\nLe jeu redémarrera en solo normal. Tes sauvegardes ne sont pas touchées ; tu pourras réinstaller le mod "
                L"en relançant depuis cette fenêtre.")
                .c_str(),
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    if (exists(dll) && !our_dll(dll)) {
        ask(L"Le dinput8.dll du dossier du jeu n'est pas celui de DDDA Coop : je n'y touche pas.", MB_ICONWARNING);
        return;
    }
    if (exists(g_game_dir + L"\\coop_progress\\pending.txt") &&
        ask(L"Ta progression gagnée en co-op n'est pas encore dans ta partie.\n\n"
            L"Pour l'ajouter : choisis « Seul », lance le jeu, charge ta partie puis sauvegarde. "
            L"Si tu retires le mod maintenant, elle reste en attente pour plus tard.\n\nRetirer le mod quand même ?",
            MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    bool ok = (!exists(dll) || DeleteFileW(dll.c_str())) && (!exists(ini) || DeleteFileW(ini.c_str()));
    g_watching = false;
    add_event(ok ? L"Mod retiré : le jeu redémarre en solo normal." : L"Impossible de supprimer dinput8.dll ou coop.ini.");
    set_state(ok ? L"Mod retiré." : L"Erreur en retirant le mod.", ok ? kMuted : kBad);
}

void on_backup()
{
    int accounts = 0;
    std::wstring dest = backup_saves(&accounts);
    if (dest.empty()) {
        ask(L"Aucune sauvegarde de Dragon's Dogma trouvée dans Steam, ou la copie a échoué.", MB_ICONWARNING);
        return;
    }
    add_event(L"Sauvegardes copiées dans " + dest);
    note_backup(dest);
    ShellExecuteW(g_wnd, L"open", dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void on_export()
{
    std::wstring log = g_game_dir + L"\\coop.log";
    if (!exists(log)) {
        ask(L"Pas encore de coop.log dans le dossier du jeu : lance le jeu avec le mod d'abord.", MB_ICONINFORMATION);
        return;
    }
    wchar_t stamp[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 32, L"%04d-%02d-%02d_%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::wstring dir = documents_dir() + L"\\DDDA Coop\\Journaux";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring dest = dir + L"\\coop-" + (guest_role() ? L"invite-" : L"hote-") + stamp + L".log";
    if (!CopyFileW(log.c_str(), dest.c_str(), FALSE)) {
        ask(L"La copie du journal a échoué.", MB_ICONERROR);
        return;
    }
    add_event(L"Journal copié : " + dest);
    std::wstring arg = L"/select,\"" + dest + L"\"";
    ShellExecuteW(g_wnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void on_browse()
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Choisis le dossier qui contient DDDA.exe";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(id, path)) {
            SetWindowTextW(g_game_edit, path);
            check_game();
            save_settings();
        }
        CoTaskMemFree(id);
    }
}

// ---- coop.log: turn the mod's lines into French states and events ----

int number_after(const std::string &line, const char *word)
{
    size_t at = line.find(word);
    return at == std::string::npos ? 0 : atoi(line.c_str() + at + strlen(word));
}

void on_log_line(const std::string &l)
{
    bool guest = guest_role();
    auto has = [&](const char *s) { return l.find(s) != std::string::npos; };
    if (has(kOurMarker))
        g_mod_loaded = true;
    if (has("steam: link up"))
        g_link_up = true;
    if (has("steam: host found"))
        g_host_found = true;
    if (has("waiting for Steam") && !g_steam_wait_since)
        g_steam_wait_since = GetTickCount() | 1;
    if (has("a player has an older version of the mod")) {
        add_event(L"Un joueur a une autre version du mod : vous ne vous voyez pas. Prenez tous le même paquet.");
        set_state(L"Un joueur a une autre version du mod.", kBad);
    } else if (has("unsupported DDDA.exe")) {
        add_event(L"Version du jeu non prise en charge : le mod s'est désactivé.");
        set_state(L"Mod désactivé : version du jeu non prise en charge.", kBad);
    } else if (has("waiting for Steam")) {
        set_state(L"En attente de Steam…", kMuted);
    } else if (has("steam: link up")) {
        add_event(L"Connecté à Steam.");
        set_state(guest ? L"Recherche de l'hôte parmi tes amis Steam… Reste sur l'écran titre."
                        : L"Prêt : en attente de tes amis. Tu peux jouer.",
                  kMuted);
    } else if (has("steam: host found")) {
        add_event(L"Hôte trouvé, réception de son monde…");
        set_state(L"Hôte trouvé. Réception de son monde… Reste sur l'écran titre.", kMuted);
    } else if (has("net: save: the host's save version") && has("arrived")) {
        if (!g_world_received)
            set_state(L"Connecté ! Appuie sur Start, puis choisis « Charger ».", kOk);
        add_event(g_world_received ? L"Monde de l'hôte mis à jour." : L"Monde de l'hôte reçu.");
        g_world_received = true;
    } else if (has("net: guest connected from")) {
        int k = number_after(l, "(guest ");
        ++g_friends;
        add_event(L"Ami " + std::to_wstring(k) + L" connecté.");
        set_state(std::to_wstring(g_friends) + (g_friends > 1 ? L" amis connectés." : L" ami connecté."), kOk);
    } else if (has("net: guest ") && has(", dropped")) {
        int k = number_after(l, "net: guest ");
        g_friends = g_friends > 0 ? g_friends - 1 : 0;
        add_event(L"Ami " + std::to_wstring(k) + L" déconnecté (plus de nouvelles depuis 10 s). Il peut revenir : il "
                  L"relance le jeu et charge, comme au début.");
        set_state(g_friends ? std::to_wstring(g_friends) + (g_friends > 1 ? L" amis connectés." : L" ami connecté.")
                            : L"En attente de tes amis. Tu peux jouer.",
                  g_friends ? kOk : kMuted);
    } else if (has("'s character arrived")) {
        int k = number_after(l, "net: guest ");
        add_event(L"Le perso de l'ami " + std::to_wstring(k) + L" est arrivé : il remplace un pion.");
    } else if (has("rejoin: idle -> saving")) {
        add_event(L"Sauvegarde et rechargement pour faire entrer un ami (quelques secondes)…");
    } else if (has("rejoin: world reloaded")) {
        add_event(L"Monde rechargé.");
    } else if (has("net: peer silent")) {
        add_event(L"Connexion à l'hôte perdue : reconnexion automatique… Si ça dure plus d'une minute, vérifie ta "
                  L"connexion internet et que l'hôte est toujours en jeu.");
        set_state(L"Connexion à l'hôte perdue, reconnexion automatique…", kWarn);
        g_world_received = false;
    } else if (has("net: stage: guest in ") && has(" jumps to ")) {
        add_event(L"Changement de zone avec l'hôte.");
    } else if (has("net: send_character=0")) {
        add_event(L"Tu joues le pion de l'hôte tel quel (sans ton propre perso).");
    } else if (has("steam: friends in Dragon's Dogma:")) {
        // " ...1234 (host) ...5678 (guest) ...9012 (no mod)" or " none"
        std::string list = l.substr(l.find("Dragon's Dogma:") + 15), fr;
        size_t at = 0;
        while ((at = list.find("...", at)) != std::string::npos) {
            std::string id = list.substr(at, 7), role = list.substr(list.find('(', at) + 1);
            role = role.substr(0, role.find(')'));
            fr += (fr.empty() ? "" : ", ") + id + " (" +
                  (role == "host" ? "hôte" : role == "guest" ? "invité" : "sans le mod") + ")";
            at += 3;
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, fr.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, fr.c_str(), -1, &w[0], n);
        add_event(fr.empty() ? L"Aucun ami Steam dans Dragon's Dogma pour l'instant."
                             : L"Amis Steam dans le jeu (fin de leur identifiant) : " + w);
    } else if (has("roam: exploring alone")) {
        add_event(L"Tu explores seul, avec ton perso et ton sac. LB + RB + Y (ou F7) pour revenir chez l'hôte.");
        set_state(L"Exploration libre : ce que tu fais ne compte que chez toi (sauf ta progression).", kInfo);
    } else if (has("roam: back with the host")) {
        add_event(L"De retour avec l'hôte.");
        set_state(L"Connecté : tu joues avec l'hôte.", kOk);
    } else if (has("roam: not now")) {
        add_event(L"Exploration : pas maintenant (dialogue, chargement ou changement de zone en cours).");
    } else if (has("explores on its own")) {
        add_event(L"Ami " + std::to_wstring(number_after(l, "net: guest ")) + L" explore seul : son pion repasse à l'IA.");
    } else if (has("is back: it drives its pawn again")) {
        add_event(L"Ami " + std::to_wstring(number_after(l, "net: guest ")) + L" est revenu : il reprend son pion.");
    } else if (has("progress: free roam:")) {
        long long exp = 0, jp = 0;
        int kinds = 0;
        size_t at = l.find("free roam: +");
        if (at != std::string::npos)
            sscanf(l.c_str() + at, "free roam: +%lld exp, +%lld discipline, %d new item", &exp, &jp, &kinds);
        add_event(L"Gagné en exploration : +" + std::to_wstring(exp) + L" exp, +" + std::to_wstring(jp) +
                  L" discipline, " + std::to_wstring(kinds) + L" sorte(s) d'objets (gardé pour ta partie).");
    } else if (has("progress: from the host:")) {
        long long exp = 0, jp = 0;
        int kinds = 0;
        size_t at = l.find("pending in total +");
        if (at != std::string::npos)
            sscanf(l.c_str() + at, "pending in total +%lld exp, +%lld discipline, %d item", &exp, &jp, &kinds);
        add_event(L"Progression gardée pour ton perso : +" + std::to_wstring(exp) + L" exp, +" + std::to_wstring(jp) +
                  L" discipline, " + std::to_wstring(kinds) + L" sorte(s) d'objets.");
    } else if (has("progress: added to ")) {
        add_event(L"Progression co-op ajoutée à ta partie (copie de secours dans coop_progress\\backups).");
    } else if (has("progress: the game saved with the gains")) {
        add_event(L"Progression co-op enregistrée dans ta sauvegarde.");
    } else if (has("progress: pending gains are for")) {
        add_event(L"Progression co-op en attente : elle appartient à un autre perso, rien n'a été ajouté.");
    }
}

void poll_log()
{
    if (!g_watching || g_game_dir.empty())
        return;
    std::wstring log = g_game_dir + L"\\coop.log";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a))
        return;
    // only the log of the game started from here (the mod rewrites it at every start)
    if (CompareFileTime(&a.ftLastWriteTime, &g_launch_time) < 0)
        return;
    long long size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (size < g_log_pos)
        g_log_pos = 0;
    std::string chunk;
    if (!read_file(log, chunk, g_log_pos))
        return;
    size_t end = chunk.rfind('\n');
    if (end == std::string::npos)
        return;
    g_log_pos += (long long)(end + 1);
    size_t start = 0;
    for (size_t nl; (nl = chunk.find('\n', start)) != std::string::npos && nl <= end; start = nl + 1)
        on_log_line(chunk.substr(start, nl - start));
}

// ---- clear messages for what did not happen ----

void tell(bool &told, const std::wstring &state, const std::wstring &event, COLORREF color)
{
    if (told)
        return;
    told = true;
    set_state(state, color);
    add_event(event);
}

// Every second after a launch: the things that should have happened and did not, in plain words.
void check_watchers()
{
    if (!g_watching || !g_launch_tick)
        return;
    DWORD since = GetTickCount() - g_launch_tick;
    bool running = game_running();
    if (running)
        g_game_seen = true;
    if (g_game_seen && !running && !g_told_closed) {
        tell(g_told_closed, L"Le jeu s'est fermé.",
             L"Le jeu s'est fermé. S'il a planté, clique sur « Exporter le journal » et envoie le fichier à l'hôte.", kWarn);
        g_watching = false;
        return;
    }
    if (g_game_seen && !g_mod_loaded && since > 90000)
        tell(g_told_no_mod, L"Le mod ne s'est pas chargé.",
             L"Le jeu tourne depuis 1 min 30 mais le mod ne s'est pas chargé : vérifie que dinput8.dll est bien dans le "
             L"dossier du jeu (relance avec « Lancer le jeu ») et qu'aucun antivirus ne l'a supprimé.",
             kBad);
    if (!g_game_seen && since > 120000)
        tell(g_told_no_mod, L"Le jeu ne démarre pas.",
             L"Le jeu n'a pas démarré en 2 minutes : vérifie que Steam est ouvert et connecté, puis relance.", kBad);
    if (g_steam_wait_since && !g_link_up && GetTickCount() - g_steam_wait_since > 30000)
        tell(g_told_steam, L"Steam ne répond pas.",
             L"Le mod attend Steam depuis 30 s : vérifie que Steam est ouvert, connecté (pas en mode hors ligne), puis "
             L"relance le jeu.",
             kBad);
    if (guest_role() && g_link_up && !g_host_found && since > 90000)
        tell(g_told_no_host, L"Hôte introuvable pour l'instant.",
             L"Aucun hôte trouvé : vérifie que l'hôte a lancé le jeu avec « L'hôte », que vous êtes amis sur Steam, et que "
             L"son statut Steam n'est pas « Invisible ». La recherche continue.",
             kWarn);
}

// The salon (coop_status.txt from the mod, every second): four seats, each player's Steam name and
// picture (coop_avatar_<place>.bmp), character, area, state and link, and the game above them.
struct Seat {
    bool used = false, me = false, down = false, host = false;
    std::wstring role, steam, character, zone, state, link;
    COLORREF link_color = 0;
    int level = 0, ping = 0; // 0 = unknown
};

Seat g_seats[4];
std::wstring g_salon_info = L"Le jeu n'est pas lancé.";
HBITMAP g_avatar[4] = {};
FILETIME g_avatar_time[4] = {};

std::wstring widen_utf8(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

// The seat's picture again when the mod wrote a newer one (or none any more).
void load_avatar(int k, bool used)
{
    std::wstring path = g_game_dir + L"\\coop_avatar_" + std::to_wstring(k) + L".bmp";
    WIN32_FILE_ATTRIBUTE_DATA a;
    bool have = used && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &g_launch_time) >= 0;
    if (!have) {
        if (g_avatar[k])
            DeleteObject(g_avatar[k]);
        g_avatar[k] = nullptr;
        return;
    }
    if (g_avatar[k] && CompareFileTime(&a.ftLastWriteTime, &g_avatar_time[k]) == 0)
        return;
    if (g_avatar[k])
        DeleteObject(g_avatar[k]);
    g_avatar[k] = HBITMAP(LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    g_avatar_time[k] = a.ftLastWriteTime;
}

void update_players()
{
    if (!g_players)
        return;
    std::string text;
    WIN32_FILE_ATTRIBUTE_DATA a;
    std::wstring path = g_game_dir + L"\\coop_status.txt";
    bool fresh = g_watching && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                 CompareFileTime(&a.ftLastWriteTime, &g_launch_time) >= 0 && read_file(path, text);
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER n, w;
    n.LowPart = now_ft.dwLowDateTime, n.HighPart = now_ft.dwHighDateTime;
    w.LowPart = fresh ? a.ftLastWriteTime.dwLowDateTime : 0, w.HighPart = fresh ? a.ftLastWriteTime.dwHighDateTime : 0;
    bool stale = !fresh || n.QuadPart - w.QuadPart > 50000000ULL; // 5 s
    int me = -1, count = 0;
    bool host_role = false;
    char mode[16] = "", link[16] = "";
    int day = -1, day_ms = -1;
    Seat seats[4];
    size_t at = 0;
    while (!stale && at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        int slot, conn, present, down, stage, same;
        long ago;
        char name[160] = {};
        if (sscanf(line.c_str(), "me %d", &me) == 1)
            continue;
        if (line == "role host")
            host_role = true;
        if (sscanf(line.c_str(), "game %15s %15s %d %d", mode, link, &day, &day_ms) >= 2)
            continue;
        int v = 0;
        unsigned long long self_id = 0;
        if (sscanf(line.c_str(), "level %d %d", &slot, &v) == 2 && slot >= 0 && slot < 4) {
            seats[slot].level = v;
            continue;
        }
        if (sscanf(line.c_str(), "ping %d %d", &slot, &v) == 2 && slot >= 0 && slot < 4) {
            seats[slot].ping = v;
            continue;
        }
        if (sscanf(line.c_str(), "self %llu", &self_id) == 1) {
            g_self_steam = self_id;
            continue;
        }
        if (sscanf(line.c_str(), "steam %d %159[^\n]", &slot, name) >= 1 && slot >= 0 && slot < 4) {
            seats[slot].steam = widen_utf8(name);
            continue;
        }
        if (sscanf(line.c_str(), "player %d %d %d %d %d %ld %d %159[^\n]", &slot, &conn, &present, &down, &stage, &ago, &same,
                   name) < 7 || !conn || slot < 0 || slot >= 4)
            continue;
        Seat &r = seats[slot];
        r.used = true;
        ++count;
        r.me = slot == me;
        r.down = present && down;
        r.role = slot == 0 ? L"HÔTE" : L"INVITÉ " + std::to_wstring(slot);
        r.host = slot == 0;
        r.character = widen_utf8(name);
        r.zone = r.me ? L"Ta zone" : !present ? L"Zone inconnue" : same ? L"Même zone"
                                                                        : L"Autre zone (n° " + std::to_wstring(stage) + L")";
        r.state = !present ? L"" : down ? L"\u26A0 À terre" : L"Debout";
        r.link = r.me ? L"Toi" : ago < 0 ? L"Connexion inconnue" : ago < 1500 ? L"Bonne connexion" : ago < 5000 ? L"Connexion lente"
                                                                                                              : L"Coupée (" + std::to_wstring(ago / 1000) + L" s)";
        r.link_color = r.me ? kInfo : ago < 0 ? kMuted : ago < 1500 ? kOk : ago < 5000 ? kWarn : kBad;
    }
    for (int k = 0; k < 4; ++k) {
        if (!seats[k].used) {
            seats[k].steam.clear();
            seats[k].level = seats[k].ping = 0;
        }
        g_seats[k] = seats[k];
        load_avatar(k, seats[k].used);
    }
    if (!g_watching) {
        g_salon_info = L"Le jeu n'est pas lancé.";
    } else if (stale || !count) {
        g_salon_info = L"Salon vide pour l'instant : il se remplit une fois en jeu.";
    } else {
        g_salon_info = std::wstring(strcmp(mode, "shared") == 0 ? L"Monde partagé" : L"Mode classique") +
                       (strcmp(link, "steam") == 0 ? L" · Steam" : L" · réseau local") + L" · " + std::to_wstring(count) +
                       L"/4 joueurs";
        if (day >= 0 && day_ms >= 0) {
            wchar_t t[64];
            _snwprintf(t, 64, L" · jour %d, %02d:%02d", day, day_ms / 3600000 % 24, day_ms / 60000 % 60);
            t[63] = 0;
            g_salon_info += t;
        }
        g_salon_info += host_role ? L" · tu es l'hôte" : L" · tu es invité";
    }
    // the same in words for screen readers (the salon draws itself)
    std::wstring words = L"Salon. " + g_salon_info;
    for (const Seat &r : g_seats)
        if (r.used)
            words += L". " + r.role + L" " + (r.steam.empty() ? r.character : r.steam) +
                     (r.level ? L", niveau " + std::to_wstring(r.level) : L"") + L", " + r.zone + L", " + r.state + L", " +
                     (r.ping ? std::to_wstring(r.ping) + L" ms" : r.link);
    SetWindowTextW(g_players, words.c_str());
    InvalidateRect(g_players, nullptr, FALSE);
    if (g_copy_code && bool(IsWindowEnabled(g_copy_code)) != (g_self_steam != 0)) {
        EnableWindow(g_copy_code, g_self_steam != 0);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

// The salon's painting: the game's line, then the four seats as cards (2 x 2): picture (or initial),
// Steam name with a star for the host, character and level, area and state, ping on the right.
void paint_salon(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_buf = SelectObject(mem, buf);
    FillRect(mem, &rc, g_bg_brush);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(mem, g_font);
    SetTextColor(mem, kMuted);
    RECT info = {0, 0, rc.right, px(22)};
    DrawTextW(mem, g_salon_info.c_str(), -1, &info, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    int top = px(32), gap = px(theme::kGapSmall);
    int cw = (rc.right - gap) / 2, ch = (rc.bottom - top - gap) / 2;
    // the initials' circles (no picture): muted tones that read on the dark theme
    static const COLORREF seat_colors[4] = {RGB(0x5a, 0x3a, 0x42), RGB(0x34, 0x4c, 0x66), RGB(0x34, 0x5a, 0x48), RGB(0x52, 0x44, 0x66)};
    for (int k = 0; k < 4; ++k) {
        const Seat &r = g_seats[k];
        RECT c = {(k % 2) * (cw + gap), top + (k / 2) * (ch + gap), 0, 0};
        c.right = c.left + cw;
        c.bottom = c.top + ch;
        if (!r.used) { // an empty seat: discreet dashes
            theme::round_rect(mem, c, px(theme::kRadiusCard), CLR_INVALID, theme::kBorder, 1.f, true);
            SetTextColor(mem, kMuted);
            SelectObject(mem, g_font);
            RECT t = c;
            DrawTextW(mem, L"Place libre", -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            continue;
        }
        theme::round_rect(mem, c, px(theme::kRadiusCard), kCard, r.me ? theme::kAccent : theme::kBorder, r.me ? 1.5f : 1.f);
        int pad = px(theme::kPadCard), av = px(56);
        RECT pic = {c.left + pad, c.top + (ch - av) / 2, c.left + pad + av, c.top + (ch - av) / 2 + av};
        // the Steam picture in a circle, or the first letter on the seat's colour
        HRGN clip = CreateEllipticRgn(pic.left, pic.top, pic.right + 1, pic.bottom + 1);
        SelectClipRgn(mem, clip);
        if (g_avatar[k]) {
            HDC src = CreateCompatibleDC(mem);
            HGDIOBJ old_src = SelectObject(src, g_avatar[k]);
            BITMAP bm;
            GetObject(g_avatar[k], sizeof bm, &bm);
            SetStretchBltMode(mem, HALFTONE);
            StretchBlt(mem, pic.left, pic.top, av, av, src, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(src, old_src);
            DeleteDC(src);
        } else {
            HBRUSH b = CreateSolidBrush(seat_colors[k]);
            FillRect(mem, &pic, b);
            DeleteObject(b);
            std::wstring who = r.steam.empty() ? r.character : r.steam;
            std::wstring letter = who.empty() ? std::to_wstring(k + 1) : who.substr(0, 1);
            SelectObject(mem, g_head);
            SetTextColor(mem, theme::kText);
            RECT t = pic;
            DrawTextW(mem, letter.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        SelectClipRgn(mem, nullptr);
        DeleteObject(clip);
        // the ping on the right, in a pill coloured by its value (or the link's words without one)
        std::wstring ping = r.me ? L"toi" : r.ping ? std::to_wstring(r.ping) + L" ms" : r.link;
        COLORREF pc = r.me ? kInfo : r.ping ? (r.ping < 90 ? kOk : r.ping < 180 ? kWarn : kBad) : r.link_color;
        SelectObject(mem, g_small);
        SIZE ps = {};
        GetTextExtentPoint32W(mem, ping.c_str(), int(ping.size()), &ps);
        RECT pill = {c.right - pad - ps.cx - px(20), c.top + pad, c.right - pad, c.top + pad + px(22)};
        theme::round_rect(mem, pill, px(11), theme::mix(kCard, pc, 0.18f));
        SetTextColor(mem, pc);
        DrawTextW(mem, ping.c_str(), -1, &pill, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        // text: name (+ host star), character and level, area and state, vertically centred
        int x = pic.right + px(14), lh = px(20);
        int ty = c.top + (ch - 3 * lh) / 2;
        RECT t = {x, ty, pill.left - px(8), ty + lh};
        SelectObject(mem, g_bold);
        SetTextColor(mem, kText);
        std::wstring title = (r.steam.empty() ? (r.character.empty() ? r.role : r.character) : r.steam) + (r.me ? L" (toi)" : L"");
        DrawTextW(mem, title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (r.host) { // the host's star after the name
            SIZE ts = {};
            GetTextExtentPoint32W(mem, title.c_str(), int(title.size()), &ts);
            RECT star = {t.left + ts.cx + px(6), t.top, t.left + ts.cx + px(26), t.bottom};
            if (star.right < t.right) {
                SelectObject(mem, g_small_icon);
                SetTextColor(mem, theme::kWarn);
                DrawTextW(mem, L"\uE735", -1, &star, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX); // FavoriteStarFill
            }
        }
        OffsetRect(&t, 0, lh);
        SelectObject(mem, g_font);
        SetTextColor(mem, kMuted);
        std::wstring line2 = r.role + (r.character.empty() ? L"" : L" \u00b7 " + r.character) +
                             (r.level ? L" \u00b7 niveau " + std::to_wstring(r.level) : L"");
        DrawTextW(mem, line2.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        OffsetRect(&t, 0, lh);
        SetTextColor(mem, r.down ? kBad : kMuted);
        std::wstring line3 = r.zone + (r.state.empty() ? L"" : L" \u00b7 " + r.state);
        DrawTextW(mem, line3.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(mem, old_font);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_buf);
    DeleteObject(buf);
    DeleteDC(mem);
}

LRESULT CALLBACK salon_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_salon(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(h, m, w, l);
}

// ---- window ----

// ---- themed controls ----
// Push buttons are owner-drawn: still real buttons for the keyboard and screen readers, painted with the
// theme. Hover fades in and out over theme::kHoverMs (a 15 ms timer while any button moves).
struct ButtonLook {
    HWND h;
    float hover;  // 0..1, shown
    bool inside;  // pointer over it
};
std::vector<ButtonLook> g_buttons;
const UINT_PTR kHoverTimer = 77;

ButtonLook *look_of(HWND h)
{
    for (ButtonLook &b : g_buttons)
        if (b.h == h)
            return &b;
    return nullptr;
}

LRESULT CALLBACK button_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_MOUSEMOVE) {
        if (ButtonLook *b = look_of(h); b && !b->inside) {
            b->inside = true;
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            SetTimer(GetParent(h), kHoverTimer, 15, nullptr);
        }
    } else if (m == WM_MOUSELEAVE) {
        if (ButtonLook *b = look_of(h))
            b->inside = false;
        SetTimer(GetParent(h), kHoverTimer, 15, nullptr);
    } else if (m == WM_ERASEBKGND) {
        return 1;
    }
    return DefSubclassProc(h, m, w, l);
}

// Timer step: every button's hover moves toward its target; the timer stops when all are there.
void hover_step(HWND parent)
{
    bool moving = false;
    float step = 15.f / float(theme::kHoverMs);
    for (ButtonLook &b : g_buttons) {
        float target = b.inside ? 1.f : 0.f;
        if (b.hover == target)
            continue;
        b.hover = b.hover < target ? (b.hover + step > target ? target : b.hover + step) : (b.hover - step < target ? target : b.hover - step);
        InvalidateRect(b.h, nullptr, FALSE);
        moving = true;
    }
    if (!moving)
        KillTimer(parent, kHoverTimer);
}

enum ButtonKind { BTN_PRIMARY, BTN_SECONDARY, BTN_GHOST, BTN_NAV, BTN_ROLE, BTN_SWITCH, BTN_DANGER };
// the role cards: icon (Segoe MDL2 Assets), title, short description
struct RoleCard {
    const wchar_t *icon, *title, *text;
};
const RoleCard kRoles[3] = {
    {L"\uE80F", L"Hôte", L"Je charge ma partie ; mes amis me rejoignent."},
    {L"\uE8FA", L"Invité", L"Je rejoins un ami : ma partie n'est ni chargée ni sauvegardée."},
    {L"\uE77B", L"Seul", L"Je joue ma partie ; la progression gagnée en co-op y est ajoutée."},
};
ButtonKind kind_of(int id);

void draw_button(const DRAWITEMSTRUCT *d)
{
    ButtonLook *lk = look_of(d->hwndItem);
    float hv = lk ? lk->hover : 0.f;
    ButtonKind kind = kind_of(int(d->CtlID));
    bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED, focus = d->itemState & ODS_FOCUS;
    RECT r = d->rcItem;
    HDC dc = d->hDC;
    if (kind == BTN_ROLE) { // a card: icon, title, description; the chosen one outlined with the accent
        int role = int(d->CtlID) == ID_GUEST ? ROLE_GUEST : int(d->CtlID) == ID_SOLO ? ROLE_SOLO : ROLE_HOST;
        bool chosen = role == g_role;
        HBRUSH bgb = CreateSolidBrush(theme::kBg);
        FillRect(dc, &r, bgb);
        DeleteObject(bgb);
        RECT card = r;
        InflateRect(&card, -px(1), -px(1));
        COLORREF fill = theme::mix(theme::kSurface, theme::kSurfaceHover, pressed ? 1.f : hv);
        theme::round_rect(dc, card, px(theme::kRadiusCard), fill, chosen ? theme::kAccent : theme::kBorder, chosen ? 2.f : 1.f);
        if (focus && !(d->itemState & ODS_NOFOCUSRECT)) {
            RECT f = card;
            InflateRect(&f, -px(4), -px(4));
            theme::round_rect(dc, f, px(theme::kRadiusCard - 4), CLR_INVALID, theme::kFocus, 1.f);
        }
        const RoleCard &rc = kRoles[role];
        int pad = px(theme::kPadCard);
        SetBkMode(dc, TRANSPARENT);
        RECT ic = {card.left + pad, card.top + pad, card.left + pad + px(32), card.top + pad + px(32)};
        theme::circle(dc, ic, chosen ? theme::kAccent : theme::kSurfaceRaised);
        HGDIOBJ old = SelectObject(dc, g_icon);
        SetTextColor(dc, theme::kText);
        DrawTextW(dc, rc.icon, -1, &ic, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, g_head);
        RECT tt = {ic.right + px(12), card.top + pad, card.right - pad, card.top + pad + px(32)};
        // the title with its Alt letter
        static const wchar_t *titles[3] = {L"&Hôte", L"&Invité", L"&Seul"};
        DrawTextW(dc, titles[role], -1, &tt, DT_LEFT | DT_SINGLELINE | DT_VCENTER | ((d->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
        SelectObject(dc, g_font);
        SetTextColor(dc, theme::kMuted);
        RECT dt = {card.left + pad, ic.bottom + px(10), card.right - pad, card.bottom - px(10)};
        DrawTextW(dc, rc.text, -1, &dt, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        return;
    }
    if (kind == BTN_SWITCH) { // label on the left, a pill switch on the right (accent when on)
        bool on = switch_on(d->hwndItem);
        std::wstring label;
        for (const Switch &w : g_switches)
            if (w.h == d->hwndItem)
                label = w.label;
        HBRUSH bgb = CreateSolidBrush(theme::kBg);
        FillRect(dc, &r, bgb);
        DeleteObject(bgb);
        RECT row = r;
        if (hv > 0 || focus)
            theme::round_rect(dc, row, px(theme::kRadiusButton), theme::mix(theme::kBg, theme::kSurface, focus ? 1.f : hv));
        if (focus && !(d->itemState & ODS_NOFOCUSRECT))
            theme::round_rect(dc, row, px(theme::kRadiusButton), CLR_INVALID, theme::kFocus, 1.f);
        int sw = px(40), sh = px(22);
        RECT pill = {r.right - px(10) - sw, (r.top + r.bottom - sh) / 2, r.right - px(10), (r.top + r.bottom + sh) / 2};
        theme::round_rect(dc, pill, sh / 2, on ? theme::kAccent : theme::kSurfaceRaised, on ? CLR_INVALID : theme::kBorder);
        int k = sh - px(6);
        RECT knob = {on ? pill.right - px(3) - k : pill.left + px(3), pill.top + px(3), 0, 0};
        knob.right = knob.left + k;
        knob.bottom = knob.top + k;
        theme::circle(dc, knob, on ? theme::kText : theme::kMuted);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? theme::kMuted : theme::kText);
        HGDIOBJ old = SelectObject(dc, g_font);
        RECT t = {r.left + px(10), r.top, pill.left - px(12), r.bottom};
        DrawTextW(dc, label.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                                                ((d->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
        SelectObject(dc, old);
        return;
    }
    if (kind == BTN_NAV) { // icon over a small label; the current screen on a surface with the accent bar
        int page = int(d->CtlID) - ID_NAV;
        bool current = page == g_page;
        HBRUSH side = CreateSolidBrush(theme::kSidebar);
        FillRect(dc, &r, side);
        DeleteObject(side);
        RECT card = r;
        InflateRect(&card, -px(8), -px(4));
        COLORREF fill = current ? theme::kSurfaceHover : theme::mix(theme::kSidebar, theme::kSurface, pressed ? 1.f : hv);
        if (current || hv > 0 || pressed)
            theme::round_rect(dc, card, px(theme::kRadiusButton), fill);
        if (current) {
            RECT bar = {r.left + px(2), card.top + px(10), r.left + px(5), card.bottom - px(10)};
            theme::round_rect(dc, bar, px(2), theme::kAccent);
        }
        if (focus && !(d->itemState & ODS_NOFOCUSRECT))
            theme::round_rect(dc, card, px(theme::kRadiusButton), CLR_INVALID, theme::kFocus, 1.5f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, current ? theme::kText : theme::kMuted);
        HGDIOBJ old = SelectObject(dc, g_icon);
        RECT ic = {card.left, card.top + px(8), card.right, card.top + px(32)};
        DrawTextW(dc, kPageIcons[page], -1, &ic, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, g_small);
        RECT lb = {r.left + px(1), card.top + px(34), r.right - px(1), card.bottom - px(4)}; // the whole width: "Sauvegardes" in Inter
        DrawTextW(dc, kPageNames[page], -1, &lb, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (page == PAGE_LOG && g_log_unseen) {
            RECT dot = {ic.right - px(26), ic.top, ic.right - px(18), ic.top + px(8)};
            theme::circle(dc, dot, theme::kWarn);
        }
        SelectObject(dc, old);
        return;
    }
    // the parent's background first (rounded corners show it)
    HBRUSH bg = CreateSolidBrush(theme::kBg);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    COLORREF fill, text = theme::kText, border = CLR_INVALID;
    if (kind == BTN_PRIMARY) {
        fill = theme::mix(theme::kAccent, theme::kAccentHover, pressed ? 0.4f : hv);
    } else if (kind == BTN_GHOST) {
        fill = theme::mix(theme::kBg, theme::kSurfaceHover, pressed ? 1.f : hv);
    } else if (kind == BTN_DANGER) { // red words and outline, a red tint under the pointer
        fill = theme::mix(theme::kSurface, theme::kBad, pressed ? 0.28f : 0.06f + 0.14f * hv);
        border = theme::mix(theme::kBorder, theme::kBad, 0.7f);
        text = theme::kBad;
    } else {
        fill = theme::mix(theme::kSurfaceRaised, theme::kSurfaceHover, pressed ? 1.f : hv);
        border = theme::kBorder;
    }
    if (disabled) {
        fill = kind == BTN_GHOST ? theme::kBg : theme::kSurface;
        text = theme::mix(theme::kMuted, theme::kBg, 0.35f);
    }
    theme::round_rect(dc, r, px(theme::kRadiusButton), fill, border);
    if (focus && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -px(2), -px(2));
        theme::round_rect(dc, f, px(theme::kRadiusButton - 2), CLR_INVALID, theme::kFocus, 1.5f);
    }
    wchar_t label[256] = L"";
    GetWindowTextW(d->hwndItem, label, 256);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HGDIOBJ old = SelectObject(dc, HFONT(SendMessageW(d->hwndItem, WM_GETFONT, 0, 0)));
    RECT t = r;
    UINT align = (GetWindowLongW(d->hwndItem, GWL_STYLE) & BS_LEFT) == BS_LEFT ? DT_LEFT : DT_CENTER;
    if (align == DT_LEFT)
        t.left += px(12);
    // Alt shows the underlined letters (DT_HIDEPREFIX until the keyboard is used)
    DrawTextW(dc, label, -1, &t, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                                     ((d->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
    SelectObject(dc, old);
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
{
    bool push = wcscmp(cls, L"BUTTON") == 0 && ((style & 0xF) == BS_PUSHBUTTON || (style & 0xF) == BS_DEFPUSHBUTTON);
    if (push)
        style = (style & ~DWORD(0xF)) | BS_OWNERDRAW;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(font ? font : g_font), TRUE);
    if (push) {
        g_buttons.push_back({c, 0.f, false});
        SetWindowSubclass(c, button_sub, 1, 0);
    } else if (wcscmp(cls, L"BUTTON") == 0) {
        SetWindowTheme(c, L"", L""); // radio buttons and check boxes: classic, so they take the theme's colours
    }
    return c;
}

ButtonKind kind_of(int id)
{
    if (id >= ID_NAV && id < ID_NAV + PAGE_COUNT)
        return BTN_NAV;
    if (id == ID_HOST || id == ID_GUEST || id == ID_SOLO)
        return BTN_ROLE;
    if (id == ID_OWN_CHAR || id == ID_KEEP || id == ID_DEBUG || id == ID_CHECK_UPDATES)
        return BTN_SWITCH;
    return id == ID_LAUNCH ? BTN_PRIMARY : id == ID_REMOVE ? BTN_DANGER : id == ID_EVENTS_TOGGLE ? BTN_GHOST : BTN_SECONDARY;
}

// Layout in 96-dpi units, scaled by px(): a thin sidebar of icons (the five screens) and one screen at a
// time, so the window fits a laptop's screen (~768 px high) without scrolling (owner's direction,
// 2026-10-29). Each screen's controls are listed in g_pages and shown or hidden together.
const int kW = 960, kH = 600, kSide = 84, kX = kSide + 28, kCW = kW - kX - 28, kHeaderH = 112;
const int kDangerY = 262; // Paramètres: the sensitive card's top
int g_height = kH;
std::vector<HWND> g_pages[PAGE_COUNT];
int g_building = PAGE_PLAY; // the screen the controls made now belong to

HWND add(HWND h)
{
    g_pages[g_building].push_back(h);
    return h;
}

void update_controls();

void show_page(int page)
{
    g_page = page;
    if (page == PAGE_LOG)
        g_log_unseen = false;
    for (int k = 0; k < PAGE_COUNT; ++k)
        for (HWND h : g_pages[k])
            ShowWindow(h, k == page ? SW_SHOW : SW_HIDE);
    for (int k = 0; k < PAGE_COUNT; ++k)
        if (HWND nav = GetDlgItem(g_wnd, ID_NAV + k))
            InvalidateRect(nav, nullptr, FALSE);
    if (page == PAGE_LOG && SendMessageW(g_events, LB_GETCOUNT, 0, 0) == 0)
        ShowWindow(g_events, SW_HIDE); // empty: the card says so
    update_controls(); // the guest's options stay hidden unless its card is chosen
    InvalidateRect(g_wnd, nullptr, TRUE);
}

// The role cards' and switches' window texts carry their state for screen readers ("&Hôte, choisi",
// "...: activé"); the drawing takes the title and the label without it.
void refresh_names()
{
    HWND cards[3] = {g_host, g_guest, g_solo};
    const wchar_t *names[3] = {L"&Hôte", L"&Invité", L"&Seul"};
    for (int k = 0; k < 3; ++k) {
        std::wstring t = std::wstring(names[k]) + (k == g_role ? L", choisi" : L"") + L". " + kRoles[k].text;
        SetWindowTextW(cards[k], t.c_str());
        InvalidateRect(cards[k], nullptr, FALSE);
    }
    for (const Switch &w : g_switches) {
        SetWindowTextW(w.h, (w.label + (w.on ? L" : activé" : L" : désactivé")).c_str());
        InvalidateRect(w.h, nullptr, FALSE);
    }
    update_controls();
}

void build_window()
{
    // the sidebar (always shown); Ctrl+1..5 and Ctrl+Tab move between screens too (the message loop)
    for (int k = 0; k < PAGE_COUNT; ++k) {
        HWND nav = make(L"BUTTON", kPageNames[k], BS_PUSHBUTTON | WS_TABSTOP | (k == 0 ? WS_GROUP : 0), 0, 12 + k * 64, kSide, 62,
                        ID_NAV + k);
        (void)nav;
    }
    const int L = kX, W = kCW;

    // ---- Jouer: the header (gradient, room for an artwork), the role, the launch, the state ----
    g_building = PAGE_PLAY;
    g_header_rect = {px(kSide), 0, px(kW), px(kHeaderH)};
    int y = kHeaderH + 16;
    add(make(L"STATIC", L"Ton rôle", 0, L, y, W, 24, 0, g_head));
    y += 32;
    int cw = (W - 2 * theme::kGapSmall) / 3, ch = 124;
    g_host = add(make(L"BUTTON", L"&Hôte", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L, y, cw, ch, ID_HOST));
    g_guest = add(make(L"BUTTON", L"&Invité", BS_PUSHBUTTON | WS_TABSTOP, L + cw + theme::kGapSmall, y, cw, ch, ID_GUEST));
    g_solo = add(make(L"BUTTON", L"&Seul", BS_PUSHBUTTON | WS_TABSTOP, L + 2 * (cw + theme::kGapSmall), y, cw, ch, ID_SOLO));
    y += ch + 10;
    // the guest's own options (shown only under its card, update_controls)
    g_own_char = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, y, W, 30, ID_OWN_CHAR));
    g_keep = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, y + 32, W, 30, ID_KEEP));
    g_switches.push_back({g_own_char, true, L"Jouer avec mon &propre perso (sinon je joue le pion de l'hôte)"});
    g_switches.push_back({g_keep, true, L"&Garder la progression de mon perso gagnée chez un ami (exp, discipline, objets)"});
    y += 68;
    g_launch = add(make(L"BUTTON", L"&Lancer le jeu", BS_DEFPUSHBUTTON | WS_TABSTOP, L, y, 300, 52, ID_LAUNCH, g_head));
    add(make(L"BUTTON", L"? A&ide", BS_PUSHBUTTON | WS_TABSTOP, L + W - 100, y + 4, 100, 44, ID_HELP));
    // shown when the check found a newer release (WM_UPDATE_FOUND)
    g_update = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L + W - 280, 34, 280, 40, ID_UPDATE));
    y += 64;
    g_state_rect = {px(L), px(y), px(L + W), px(y + 56)};
    g_state = add(make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, L + 28, y + 8, W - 40, 40, 0, g_bold));
    set_state(L"Choisis ton rôle puis lance le jeu.", kMuted);

    // ---- Salon ----
    g_building = PAGE_SALON;
    g_players = add(make(L"DDDACoopSalon", L"Salon", 0, L, 76, W, 296, ID_PLAYERS));
    // the connection block under the cards (drawn by paint; controls by role, update_controls)
    g_copy_code = add(make(L"BUTTON", L"&Copier le code", BS_PUSHBUTTON | WS_TABSTOP, L + W - 170 - 18, 444, 170, 38, ID_COPY_CODE));
    g_code_edit = add(make(L"EDIT", setting(L"host_code", L"").c_str(), WS_BORDER | WS_TABSTOP | ES_UPPERCASE | ES_AUTOHSCROLL, L + 18,
                           444, 200, 36, ID_CODE_EDIT, g_code_font));
    SendMessageW(g_code_edit, EM_LIMITTEXT, 12, 0);
    SendMessageW(g_code_edit, EM_SETCUEBANNER, TRUE, LPARAM(L"ABC-DEFG"));
    g_code_clear = add(make(L"BUTTON", L"&Effacer", BS_PUSHBUTTON | WS_TABSTOP, L + 230, 444, 110, 36, ID_CODE_CLEAR));

    // ---- Sauvegardes ----
    g_building = PAGE_SAVES;
    add(make(L"STATIC",
             L"Une copie de secours de tes sauvegardes Steam de Dragon's Dogma est faite à chaque lancement. Tu peux en faire une "
             L"tout de suite.",
             0, L, 76, W, 44, 0));
    add(make(L"BUTTON", L"Copier mes s&auvegardes", BS_PUSHBUTTON | WS_TABSTOP, L, 126, 230, 40, ID_BACKUP));
    add(make(L"BUTTON", L"&Ouvrir le dossier", BS_PUSHBUTTON | WS_TABSTOP, L + 242, 126, 180, 40, ID_OPEN_BACKUPS));
    g_backup_note = add(make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, L, 178, W, 22, 0));
    show_backup_note();
    add(make(L"STATIC", L"Copies de secours (double-clic : ouvrir)", 0, L, 214, W, 22, 0, g_bold));
    g_backup_list = add(make(L"LISTBOX", L"Copies de secours", WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY |
                                                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                             L + 1, 244, W - 2, kH - 244 - 28, ID_BACKUP_LIST));
    SetWindowTheme(g_backup_list, L"DarkMode_Explorer", nullptr); // dark scroll bars (Windows 10 1809+)
    refresh_backups();

    // ---- Paramètres ----
    g_building = PAGE_SETTINGS;
    add(make(L"STATIC", L"Dossier du jeu", 0, L, 76, W, 22, 0, g_bold));
    g_game_edit = add(make(L"EDIT", g_game_dir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, L, 102, W - 132, 30, ID_GAME_EDIT));
    add(make(L"BUTTON", L"&Changer…", BS_PUSHBUTTON | WS_TABSTOP, L + W - 120, 102, 120, 30, ID_BROWSE));
    g_version = add(make(L"STATIC", L"", 0, L, 140, W, 22, 0));
    g_debug = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, 176, W, 32, ID_DEBUG));
    g_switches.push_back({g_debug, true, L"&Journal détaillé (utile pour signaler un problème)"});
    g_check_updates = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, 212, W, 32, ID_CHECK_UPDATES));
    g_switches.push_back({g_check_updates, true, L"&Vérifier les mises à jour au démarrage (GitHub)"});
    // the sensitive part in a card of its own (painted), red-tinted
    add(make(L"STATIC", L"Zone sensible", 0, L + 18, kDangerY + 14, W - 36, 22, 0, g_bold));
    add(make(L"STATIC",
             L"Retire dinput8.dll et coop.ini du dossier du jeu : le jeu redémarre en solo normal. Tes sauvegardes ne sont pas "
             L"touchées. Une confirmation est demandée.",
             0, L + 18, kDangerY + 40, W - 36 - 200, 44, 0));
    add(make(L"BUTTON", L"&Retirer le mod", BS_PUSHBUTTON | WS_TABSTOP, L + W - 18 - 180, kDangerY + 38, 180, 40, ID_REMOVE));
    // credits
    int ay = kDangerY + 96 + 22;
    add(make(L"STATIC", L"À propos", 0, L, ay, W, 22, 0, g_bold));
    add(make(L"STATIC",
             (L"DDDA Coop v" + widen(DDDA_COOP_VERSION) +
              L", par clepix21.\nTesteurs : GrosGadjo et yaybba.\nDéveloppé avec l'aide de Claude (Anthropic).\n"
              L"Police Inter (SIL Open Font License 1.1) et MinHook (licence BSD) : voir LICENCES.txt. "
              L"Mod non officiel, sans lien avec Capcom ; Dragon's Dogma: Dark Arisen © Capcom.")
                 .c_str(),
             SS_NOPREFIX, L, ay + 28, W - 200, 88, 0));
    add(make(L"BUTTON", L"Page du &projet", BS_PUSHBUTTON | WS_TABSTOP, L + W - 180, ay + 30, 180, 40, ID_PROJECT));

    // ---- Journal ----
    g_building = PAGE_LOG;
    g_events = add(make(L"LISTBOX", L"Journal", WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_NOSEL | LBS_OWNERDRAWFIXED |
                                                   LBS_HASSTRINGS,
                        L + 8, 110, W - 16, kH - 110 - 84, ID_EVENTS));
    SetWindowTheme(g_events, L"DarkMode_Explorer", nullptr);
    add(make(L"BUTTON", L"E&xporter le journal", BS_PUSHBUTTON | WS_TABSTOP, L, kH - 60, 200, 40, ID_EXPORT));

    std::wstring role = setting(L"role", L"host"); // the last role chosen
    g_role = role == L"guest" ? ROLE_GUEST : role == L"solo" ? ROLE_SOLO : ROLE_HOST;
    g_switches[0].on = setting(L"own_character", L"1") == L"1";
    g_switches[1].on = setting(L"keep_progress", L"1") == L"1";
    g_switches[2].on = setting(L"debug_log", L"1") == L"1";
    g_switches[3].on = setting(L"check_updates", L"1") == L"1";
    refresh_names();
    update_players();
    show_page(PAGE_PLAY);
}

// Kept for the event code: the journal is its own screen now, nothing to move.
void layout_events() {}

void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT side = {0, 0, px(kSide), px(kH)};
    HBRUSH sb = CreateSolidBrush(theme::kSidebar);
    FillRect(dc, &side, sb);
    DeleteObject(sb);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g_title);
    if (g_page == PAGE_PLAY) {
        {
            Gdiplus::Graphics g(dc);
            Gdiplus::LinearGradientBrush grad(Gdiplus::Point(0, INT(g_header_rect.top)), Gdiplus::Point(0, INT(g_header_rect.bottom)),
                                              theme::gp(RGB(0x2a, 0x1c, 0x22)), theme::gp(theme::kBg));
            g.FillRectangle(&grad, INT(g_header_rect.left), INT(g_header_rect.top), INT(g_header_rect.right - g_header_rect.left),
                            INT(g_header_rect.bottom - g_header_rect.top));
        }
        SetTextColor(dc, kText);
        RECT t = {px(kX), px(22), px(kX + kCW), px(56)};
        DrawTextW(dc, L"DDDA Coop", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        // "✓ DDDA détecté · Mod v0.9.0" (or what is wrong, in its colour)
        SelectObject(dc, g_font);
        std::wstring mod = L"Mod v" + widen(DDDA_COOP_VERSION);
        std::wstring line = g_game_state == 2 ? L"\u2714  DDDA détecté \u00b7 " + mod
                            : g_game_state == 1 ? L"\u2716  Version de DDDA.exe non prise en charge \u00b7 " + mod
                                                : L"\u2716  DDDA.exe introuvable : choisis son dossier dans Paramètres \u00b7 " + mod;
        SetTextColor(dc, g_game_state == 2 ? theme::kOk : theme::kBad);
        RECT st = {px(kX), px(60), px(kX + kCW), px(84)};
        DrawTextW(dc, line.c_str(), -1, &st, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        theme::round_rect(dc, g_state_rect, px(theme::kRadiusCard), theme::mix(kCard, g_state_color, 0.08f),
                          theme::mix(theme::kBorder, g_state_color, 0.35f));
        RECT bar = {g_state_rect.left + px(12), g_state_rect.top + px(14), g_state_rect.left + px(16), g_state_rect.bottom - px(14)};
        theme::round_rect(dc, bar, px(2), g_state_color);
    } else {
        SetTextColor(dc, kText);
        RECT t = {px(kX), px(22), px(kX + kCW), px(58)};
        DrawTextW(dc, kPageNames[g_page], -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (g_page == PAGE_SALON) { // the connection block
            RECT card = {px(kX), px(384), px(kX + kCW), px(500)};
            theme::round_rect(dc, card, px(theme::kRadiusCard), kCard, theme::kBorder);
            SelectObject(dc, g_head);
            SetTextColor(dc, kText);
            RECT t = {card.left + px(18), card.top + px(12), card.right - px(18), card.top + px(40)};
            DrawTextW(dc, L"Connexion", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, g_font);
            SetTextColor(dc, kMuted);
            RECT hint = {card.left + px(18), card.top + px(40), card.right - px(18), card.top + px(58)};
            if (g_role == ROLE_HOST) {
                DrawTextW(dc, L"Donne ce code à tes amis : ils le collent dans leur salon pour te rejoindre toi précisément.", -1,
                          &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT code = {card.left + px(18), px(444), card.right - px(220), px(482)};
                SelectObject(dc, g_code_font);
                SetTextColor(dc, g_self_steam ? kText : kMuted);
                std::wstring c = g_self_steam ? code_from_steam(g_self_steam) : L"–––-––––";
                DrawTextW(dc, c.c_str(), -1, &code, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                if (!g_self_steam) {
                    SelectObject(dc, g_small);
                    RECT why = {code.left + px(150), code.top, code.right, code.bottom};
                    DrawTextW(dc, L"lance le jeu en hôte (Steam) pour l'obtenir", -1, &why, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                }
            } else if (g_role == ROLE_GUEST) {
                DrawTextW(dc, L"Le code de ton hôte pour le rejoindre lui précisément ; vide : le premier ami Steam qui héberge.",
                          -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                std::wstring txt = window_text(g_code_edit);
                uint64_t id = 0;
                bool ok = steam_from_code(txt, id);
                RECT v = {px(kX + 356), px(444), card.right - px(18), px(480)};
                SetTextColor(dc, txt.empty() ? kMuted : ok ? kOk : kBad);
                DrawTextW(dc, txt.empty() ? L"●  recherche automatique" : ok ? L"✔  code valide" : L"✖  code incomplet ou invalide",
                          -1, &v, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            } else {
                DrawTextW(dc, L"Seul : pas de connexion.", -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
        }
        if (g_page == PAGE_SETTINGS) { // the sensitive part: a card with a red outline
            RECT card = {px(kX), px(kDangerY), px(kX + kCW), px(kDangerY + 96)};
            theme::round_rect(dc, card, px(theme::kRadiusCard), theme::mix(kCard, kBad, 0.05f), theme::mix(theme::kBorder, kBad, 0.55f));
        }
        if (g_page == PAGE_SAVES) { // the copies' list in a card
            RECT card = {px(kX - 1), px(240), px(kX + kCW + 1), px(kH - 24)};
            theme::round_rect(dc, card, px(theme::kRadiusSmall + 2), kCard, theme::kBorder);
            if (g_backup_dirs.empty()) {
                SelectObject(dc, g_font);
                SetTextColor(dc, kMuted);
                RECT e = card;
                DrawTextW(dc, L"Aucune copie pour l'instant.", -1, &e, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        }
        if (g_page == PAGE_LOG) { // the counts by kind, then the events in a card
            int n = int(SendMessageW(g_events, LB_GETCOUNT, 0, 0)), warn = 0, bad = 0;
            for (int i = 0; i < n; ++i) {
                int kind = int(SendMessageW(g_events, LB_GETITEMDATA, i, 0)) & 3;
                warn += kind == 2;
                bad += kind == 3;
            }
            SelectObject(dc, g_font);
            std::wstring counts = std::to_wstring(n) + (n > 1 ? L" événements" : L" événement");
            SetTextColor(dc, kMuted);
            RECT c = {px(kX), px(74), px(kX + kCW), px(98)};
            DrawTextW(dc, counts.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SIZE cs = {};
            GetTextExtentPoint32W(dc, counts.c_str(), int(counts.size()), &cs);
            c.left += cs.cx + px(16);
            std::wstring w = L"\u26A0 " + std::to_wstring(warn) + (warn > 1 ? L" avertissements" : L" avertissement");
            SetTextColor(dc, warn ? kWarn : kMuted);
            DrawTextW(dc, w.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            GetTextExtentPoint32W(dc, w.c_str(), int(w.size()), &cs);
            c.left += cs.cx + px(16);
            std::wstring e = L"\u2716 " + std::to_wstring(bad) + (bad > 1 ? L" erreurs" : L" erreur");
            SetTextColor(dc, bad ? kBad : kMuted);
            DrawTextW(dc, e.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT card = {px(kX), px(104), px(kX + kCW), px(kH - 74)};
            theme::round_rect(dc, card, px(theme::kRadiusCard), kCard, theme::kBorder);
            if (n == 0) {
                SetTextColor(dc, kMuted);
                RECT ec = card;
                DrawTextW(dc, L"Rien pour l'instant : les événements arrivent au lancement du jeu.", -1, &ec,
                          DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        }
    }
    SelectObject(dc, old);
    EndPaint(h, &ps);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        build_window();
        SetTimer(h, ID_TIMER, 1000, nullptr);
        ShowWindow(g_update, SW_HIDE);
        if (switch_on(g_check_updates))
            if (HANDLE t = CreateThread(nullptr, 0, check_updates, nullptr, 0, nullptr))
                CloseHandle(t);
        if (setting(L"help_seen", L"0") != L"1") { // the help once by itself, at the first start
            WritePrivateProfileStringW(L"launcher", L"help_seen", L"1", settings_path().c_str());
            PostMessageW(h, WM_COMMAND, ID_HELP, 0);
        }
        return 0;
    case WM_UPDATE_FOUND:
        SetWindowTextW(g_update, (L"Mise à jour " + g_update_tag + L" disponible  ›").c_str());
        if (g_page == PAGE_PLAY)
            ShowWindow(g_update, SW_SHOW);
        add_event(L"Une nouvelle version du mod est disponible : " + g_update_tag + L" (bouton en haut de « Jouer »).");
        return 0;
    case WM_MEASUREITEM:
        if (w == ID_BACKUP_LIST) {
            reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = UINT(px(30));
            return TRUE;
        }
        if (w == ID_EVENTS) {
            reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = UINT(px(20));
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (w == ID_BACKUP_LIST) {
            draw_backup(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        if (w == ID_EVENTS) {
            draw_event(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        if (reinterpret_cast<const DRAWITEMSTRUCT *>(l)->CtlType == ODT_BUTTON) {
            draw_button(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        break;
    case DM_GETDEFID: // Enter launches the game, as the default button did
        return MAKELRESULT(ID_LAUNCH, DC_HASDEFID);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(HDC(w), kText);
        SetBkColor(HDC(w), kCard);
        return LRESULT(g_card_brush);
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= ID_NAV && LOWORD(w) < ID_NAV + PAGE_COUNT) {
            show_page(LOWORD(w) - ID_NAV);
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_BROWSE: on_browse(); break;
        case ID_HOST:
        case ID_GUEST:
        case ID_SOLO:
            g_role = LOWORD(w) == ID_GUEST ? ROLE_GUEST : LOWORD(w) == ID_SOLO ? ROLE_SOLO : ROLE_HOST;
            refresh_names();
            save_settings();
            break;
        case ID_KEEP:
        case ID_OWN_CHAR:
        case ID_DEBUG:
        case ID_CHECK_UPDATES:
            for (Switch &sw : g_switches)
                if (sw.h == HWND(l))
                    sw.on = !sw.on;
            refresh_names();
            save_settings();
            break;
        case ID_LAUNCH: on_launch(); break;
        case ID_REMOVE: on_remove(); break;
        case ID_BACKUP: on_backup(); break;
        case ID_HELP: show_help(); break;
        case ID_UPDATE: // the release's page, to download it
            ShellExecuteW(h, L"open", (std::wstring(kRepoUrl) + L"/releases/tag/" + g_update_tag).c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        case ID_PROJECT: ShellExecuteW(h, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_COPY_CODE:
            if (g_self_steam && OpenClipboard(h)) {
                std::wstring c = code_from_steam(g_self_steam);
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (c.size() + 1) * sizeof(wchar_t))) {
                    memcpy(GlobalLock(mem), c.c_str(), (c.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                }
                CloseClipboard();
                add_event(L"Code de connexion copié : " + c);
            }
            break;
        case ID_CODE_CLEAR:
            SetWindowTextW(g_code_edit, L"");
            break;
        case ID_CODE_EDIT:
            if (HIWORD(w) == EN_CHANGE) {
                WritePrivateProfileStringW(L"launcher", L"host_code", window_text(g_code_edit).c_str(), settings_path().c_str());
                RECT card = {px(kX), px(384), px(kX + kCW), px(500)};
                InvalidateRect(h, &card, FALSE);
            }
            break;
        case ID_OPEN_BACKUPS: { // the chosen copy, else the copies' folder
            int i = int(SendMessageW(g_backup_list, LB_GETCURSEL, 0, 0));
            std::wstring dir = i >= 0 && i < int(g_backup_dirs.size()) ? g_backup_dirs[size_t(i)] : documents_dir() + L"\\DDDA Coop\\Sauvegardes";
            if (!is_dir(dir))
                dir = documents_dir() + L"\\DDDA Coop";
            ShellExecuteW(g_wnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_BACKUP_LIST:
            if (HIWORD(w) == LBN_DBLCLK)
                SendMessageW(h, WM_COMMAND, ID_OPEN_BACKUPS, 0);
            break;
        case ID_EXPORT: on_export(); break;
        case ID_GAME_EDIT:
            if (HIWORD(w) == EN_KILLFOCUS && window_text(g_game_edit) != g_game_dir) {
                check_game();
                save_settings();
            }
            break;
        }
        return 0;
    case WM_TIMER:
        if (w == kHoverTimer) {
            hover_step(h);
            return 0;
        }
        poll_log();
        check_watchers();
        update_players();
        return 0;
    case WM_CTLCOLORSTATIC: // labels, radio buttons and check boxes on the light background
        if (HWND(l) == g_state) {
            SetTextColor(HDC(w), g_state_color);
            SetBkColor(HDC(w), theme::mix(theme::kSurface, g_state_color, 0.08f));
            return LRESULT(g_state_brush);
        }
        if (g_page == PAGE_SETTINGS) { // labels inside the sensitive card
            RECT rc;
            GetWindowRect(HWND(l), &rc);
            MapWindowPoints(nullptr, h, reinterpret_cast<POINT *>(&rc), 2);
            if (rc.top >= px(kDangerY) && rc.bottom <= px(kDangerY + 96)) {
                static HBRUSH danger = CreateSolidBrush(theme::mix(theme::kSurface, theme::kBad, 0.05f));
                SetTextColor(HDC(w), rc.top < px(kDangerY + 36) ? kText : kMuted);
                SetBkColor(HDC(w), theme::mix(theme::kSurface, theme::kBad, 0.05f));
                return LRESULT(danger);
            }
        }
        SetTextColor(HDC(w), HWND(l) == g_version || HWND(l) == g_backup_note ? kMuted : kText);
        SetBkColor(HDC(w), kBg);
        return LRESULT(g_bg_brush);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int CALLBACK font_found(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM l)
{
    *reinterpret_cast<bool *>(l) = true;
    return 0;
}

// Inter's three weights from the exe's resources (201-203), for this process only.
bool load_inter()
{
    for (int id = 201; id <= 203; ++id) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
        HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
        void *data = g ? LockResource(g) : nullptr;
        DWORD fonts = 0;
        if (!data || !AddFontMemResourceEx(data, SizeofResource(nullptr, r), nullptr, &fonts) || !fonts)
            return false;
    }
    return true;
}

// The theme's font if Windows has it (Segoe UI Variable: Windows 11), else its fallback.
const wchar_t *font_family()
{
    static const wchar_t *chosen = nullptr;
    if (chosen)
        return chosen;
    LOGFONTW want = {};
    want.lfCharSet = DEFAULT_CHARSET;
    wcscpy(want.lfFaceName, theme::kFontFamily);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &want, font_found, LPARAM(&found), 0);
    ReleaseDC(nullptr, dc);
    chosen = found ? theme::kFontFamily : theme::kFontFallback;
    return chosen;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exe_dir = exe;
    g_exe_dir.resize(g_exe_dir.rfind(L'\\'));

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game_arg, steam_arg;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc)
            game_arg = argv[++i];
        else if (a == L"--steam" && i + 1 < argc)
            steam_arg = argv[++i];
        else if (a == L"--docs" && i + 1 < argc)
            g_docs_dir = argv[++i];
        else if (a == L"--no-launch")
            g_no_launch = true;
        else if (a == L"--test-update" && i + 1 < argc) // tests: as if GitHub had this release, no network
            g_test_update = argv[++i];
    }
    LocalFree(argv);
    g_steam_dir = steam_arg.empty() ? steam_dir() : steam_arg;
    g_game_dir = !game_arg.empty() ? game_arg : setting(L"game", L"");
    if (g_game_dir.empty())
        g_game_dir = find_game(g_steam_dir);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    // 10 pt (a little larger than the system's 9 pt), headings 12 pt, title 18 pt
    static bool inter = load_inter();
    auto font = [](int pt, int weight) {
        LOGFONTW f = {};
        f.lfHeight = -MulDiv(pt, g_dpi, 72);
        f.lfWeight = weight;
        f.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(f.lfFaceName, !inter ? font_family() : weight == FW_SEMIBOLD ? theme::kFontInterSemiBold : theme::kFontInter);
        return CreateFontIndirectW(&f);
    };
    g_font = font(theme::kSizeBody, FW_NORMAL);
    g_bold = font(theme::kSizeBody, FW_SEMIBOLD);
    g_head = font(theme::kSizeTitle, FW_SEMIBOLD);
    g_title = font(theme::kSizeDisplay, FW_BOLD);
    g_small = font(theme::kSizeSmall, FW_NORMAL);
    {
        LOGFONTW f = {};
        f.lfHeight = -MulDiv(14, g_dpi, 72);
        f.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(f.lfFaceName, L"Segoe MDL2 Assets");
        g_icon = CreateFontIndirectW(&f);
        f.lfHeight = -MulDiv(10, g_dpi, 72);
        g_small_icon = CreateFontIndirectW(&f);
        LOGFONTW c = {};
        c.lfHeight = -MulDiv(16, g_dpi, 72);
        c.lfWeight = FW_SEMIBOLD;
        c.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(c.lfFaceName, L"Consolas"); // the code: a fixed width, 0 and O apart
        g_code_font = CreateFontIndirectW(&c);
    }
    g_bg_brush = CreateSolidBrush(kBg);
    g_card_brush = CreateSolidBrush(kCard);

    WNDCLASSW sc = {};
    sc.lpfnWndProc = salon_proc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.lpszClassName = L"DDDACoopSalon";
    RegisterClassW(&sc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = L"DDDACoopLauncher";
    RegisterClassW(&wc);
    RECT r = {0, 0, MulDiv(kW, g_dpi, 96), MulDiv(kH, g_dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT wr;
    GetWindowRect(h, &wr);
    RECT need = {0, 0, MulDiv(kW, g_dpi, 96), MulDiv(g_height, g_dpi, 96)};
    AdjustWindowRect(&need, style, FALSE);
    SetWindowPos(h, nullptr, 0, 0, need.right - need.left, need.bottom - need.top, SWP_NOMOVE | SWP_NOZORDER);
    BOOL dark = TRUE; // a dark title bar to match (DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 2004+)
    DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
    // Alt letters and focus rings hidden until the keyboard is used, as Windows does in dialogs
    SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEACCEL | UISF_HIDEFOCUS), 0);
    ShowWindow(h, show);
    check_game();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            int to = -1;
            if (msg.wParam >= '1' && msg.wParam < '1' + PAGE_COUNT)
                to = int(msg.wParam - '1');
            else if (msg.wParam == VK_TAB)
                to = (g_page + ((GetKeyState(VK_SHIFT) & 0x8000) ? PAGE_COUNT - 1 : 1)) % PAGE_COUNT;
            if (to >= 0) {
                show_page(to);
                SetFocus(GetDlgItem(h, ID_NAV + to));
                continue;
            }
        }
        if (IsDialogMessageW(h, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
