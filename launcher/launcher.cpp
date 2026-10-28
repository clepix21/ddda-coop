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
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

const wchar_t kTitle[] = L"DDDA Coop";
const char kSupportedSha[] = "19FACC2642F79A09D14CE55EACE5F228161F2F8BAB60EC67EB3C6196CC6277B8";
const char kOurMarker[] = "ddda-pawn-coop loaded"; // a string only our dinput8.dll holds

enum {
    ID_GAME_EDIT = 100, ID_BROWSE, ID_HOST, ID_GUEST, ID_OWN_CHAR, ID_DEBUG, ID_LAUNCH, ID_REMOVE,
    ID_BACKUP, ID_EXPORT, ID_EVENTS, ID_SOLO, ID_KEEP, ID_TIMER, ID_PLAYERS
};

HWND g_wnd, g_game_edit, g_version, g_host, g_guest, g_solo, g_own_char, g_keep, g_debug, g_launch, g_state, g_events,
    g_players;
HFONT g_font, g_bold, g_head, g_title;
HBRUSH g_bg_brush, g_card_brush;
int g_dpi = 96;
RECT g_state_rect = {}, g_header_rect = {};

// colours: dark text on a light background (contrast above 7:1), the state also in words and a symbol
const COLORREF kBg = RGB(246, 247, 250), kCard = RGB(255, 255, 255), kText = RGB(28, 32, 40), kMuted = RGB(84, 90, 102),
               kAccent = RGB(122, 26, 26), kOk = RGB(24, 110, 48), kWarn = RGB(150, 82, 0), kBad = RGB(170, 24, 24),
               kInfo = RGB(30, 80, 150);
int px(int v) { return MulDiv(v, g_dpi, 96); }
std::wstring g_exe_dir, g_game_dir, g_steam_dir, g_docs_dir;
bool g_no_launch = false;
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

void save_settings()
{
    auto p = settings_path();
    WritePrivateProfileStringW(L"launcher", L"game", g_game_dir.c_str(), p.c_str());
    WritePrivateProfileStringW(L"launcher", L"role", SendMessageW(g_guest, BM_GETCHECK, 0, 0)  ? L"guest"
                                                     : SendMessageW(g_solo, BM_GETCHECK, 0, 0) ? L"solo"
                                                                                               : L"host",
                               p.c_str());
    WritePrivateProfileStringW(L"launcher", L"keep_progress", SendMessageW(g_keep, BM_GETCHECK, 0, 0) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"own_character", SendMessageW(g_own_char, BM_GETCHECK, 0, 0) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"debug_log", SendMessageW(g_debug, BM_GETCHECK, 0, 0) ? L"1" : L"0", p.c_str());
}

// ---- state shown in the window ----

void set_state(const std::wstring &text, COLORREF color)
{
    g_state_color = color;
    // the meaning is in the words and the symbol too, not only in the colour
    const wchar_t *sym = color == kOk ? L"\u2714  " : color == kBad ? L"\u2716  " : color == kWarn ? L"\u26A0  " : L"\u25CF  ";
    SetWindowTextW(g_state, (sym + text).c_str());
    InvalidateRect(g_state, nullptr, TRUE);
    InvalidateRect(g_wnd, &g_state_rect, TRUE);
}

void add_event(const std::wstring &text)
{
    wchar_t stamp[16];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 16, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    int i = int(SendMessageW(g_events, LB_ADDSTRING, 0, LPARAM((stamp + text).c_str())));
    SendMessageW(g_events, LB_SETTOPINDEX, i, 0);
}

bool guest_role() { return SendMessageW(g_guest, BM_GETCHECK, 0, 0) == BST_CHECKED; }
bool solo_role() { return SendMessageW(g_solo, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void update_controls()
{
    EnableWindow(g_own_char, guest_role());
    EnableWindow(g_launch, g_supported);
}

void check_game()
{
    g_game_dir = window_text(g_game_edit);
    g_supported = false;
    if (g_game_dir.empty() || !exists(g_game_dir + L"\\DDDA.exe")) {
        SetWindowTextW(g_version, L"DDDA.exe introuvable dans ce dossier.");
    } else {
        SetWindowTextW(g_version, L"Vérification de DDDA.exe…");
        UpdateWindow(g_version);
        g_supported = sha256_file(g_game_dir + L"\\DDDA.exe") == kSupportedSha;
        SetWindowTextW(g_version, g_supported ? L"Version du jeu prise en charge."
                                              : L"Version de DDDA.exe non prise en charge : le mod ne marcherait pas.");
    }
    update_controls();
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
    const char *ours[] = {"role=", "#role=", "transport=", "debug_log=", "send_character=", "keep_progress=",
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
    out += SendMessageW(g_keep, BM_GETCHECK, 0, 0) ? "keep_progress=1\n" : "keep_progress=0\n";
    out += SendMessageW(g_debug, BM_GETCHECK, 0, 0) ? "debug_log=1\n" : "debug_log=0\n";
    if (guest && !SendMessageW(g_own_char, BM_GETCHECK, 0, 0))
        out += "send_character=0\n";
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
    if (!backup.empty())
        add_event(L"Sauvegardes copiées dans " + backup);
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
    if (has("unsupported DDDA.exe")) {
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

// coop_status.txt (the mod, every second): one line per player.
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
    int me = -1;
    struct Row {
        std::wstring who, zone, state, link;
    };
    std::vector<Row> rows;
    size_t at = 0;
    while (!stale && at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        int slot, conn, present, down, stage, same;
        long ago;
        char name[128] = {};
        if (sscanf(line.c_str(), "me %d", &me) == 1)
            continue;
        if (sscanf(line.c_str(), "player %d %d %d %d %d %ld %d %127[^\n]", &slot, &conn, &present, &down, &stage, &ago, &same,
                   name) < 7 || !conn)
            continue;
        Row r;
        r.who = slot == 0 ? L"Hôte" : L"Invité " + std::to_wstring(slot);
        if (name[0])
            r.who += L" (" + widen(name) + L")";
        if (slot == me)
            r.who += L" \u2013 toi";
        r.zone = slot == me ? L"Ta zone" : !present ? L"Inconnue" : same ? L"Même zone que toi"
                                                                         : L"Autre zone (n° " + std::to_wstring(stage) + L")";
        r.state = !present ? L"\u2013" : down ? L"\u26A0 À terre" : L"Debout";
        r.link = slot == me ? L"\u2013" : ago < 0 ? L"Inconnue" : ago < 1500 ? L"Bonne" : ago < 5000 ? L"Lente"
                                                                                                    : L"Coupée (" + std::to_wstring(ago / 1000) + L" s)";
        rows.push_back(r);
    }
    SendMessageW(g_players, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_players);
    if (rows.empty()) {
        LVITEMW it = {LVIF_TEXT};
        it.pszText = const_cast<wchar_t *>(g_watching ? L"Personne pour l'instant (la liste se remplit en jeu)." : L"Le jeu n'est pas lancé.");
        ListView_InsertItem(g_players, &it);
    }
    for (size_t k = 0; k < rows.size(); ++k) {
        LVITEMW it = {LVIF_TEXT};
        it.iItem = int(k);
        it.pszText = &rows[k].who[0];
        ListView_InsertItem(g_players, &it);
        ListView_SetItemText(g_players, int(k), 1, &rows[k].zone[0]);
        ListView_SetItemText(g_players, int(k), 2, &rows[k].state[0]);
        ListView_SetItemText(g_players, int(k), 3, &rows[k].link[0]);
    }
    SendMessageW(g_players, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_players, nullptr, TRUE);
}

// ---- window ----

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(font ? font : g_font), TRUE);
    return c;
}

const int kW = 600, kL = 24; // layout in 96-dpi units, scaled by px()
int g_height = 0;

void build_window()
{
    const int L = kL, W = kW - 2 * kL;
    g_header_rect = {0, 0, px(kW), px(64)};
    int y = 80;
    make(L"STATIC", L"1.  Le jeu", 0, L, y, W, 24, 0, g_head);
    y += 30;
    g_game_edit = make(L"EDIT", g_game_dir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, L, y, W - 120, 28, ID_GAME_EDIT);
    make(L"BUTTON", L"&Changer…", BS_PUSHBUTTON | WS_TABSTOP, L + W - 110, y, 110, 28, ID_BROWSE);
    y += 34;
    g_version = make(L"STATIC", L"", 0, L, y, W, 22, 0);
    y += 36;

    make(L"STATIC", L"2.  Ton rôle", 0, L, y, W, 24, 0, g_head);
    y += 30;
    g_host = make(L"BUTTON", L"L'&hôte : je charge ma partie, mes amis me rejoignent",
                  BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, L, y, W, 26, ID_HOST);
    y += 28;
    g_guest = make(L"BUTTON", L"Un &invité : je rejoins un ami (ma partie n'est ni chargée ni sauvegardée)",
                   BS_AUTORADIOBUTTON, L, y, W, 26, ID_GUEST);
    y += 28;
    g_own_char = make(L"BUTTON", L"Jouer avec mon &propre perso (sinon je joue le pion de l'hôte)",
                      BS_AUTOCHECKBOX | WS_TABSTOP, L + 24, y, W - 24, 26, ID_OWN_CHAR);
    y += 28;
    g_solo = make(L"BUTTON", L"&Seul : je joue ma partie (ma progression gagnée en co-op y est ajoutée)",
                  BS_AUTORADIOBUTTON, L, y, W, 26, ID_SOLO);
    y += 34;
    g_keep = make(L"BUTTON", L"&Garder la progression de mon perso gagnée chez un ami (exp, discipline, objets)",
                  BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, L, y, W, 26, ID_KEEP);
    y += 28;
    g_debug = make(L"BUTTON", L"&Journal détaillé (utile pour signaler un problème)", BS_AUTOCHECKBOX | WS_TABSTOP, L, y,
                   W, 26, ID_DEBUG);
    y += 38;

    make(L"STATIC", L"3.  Jouer", 0, L, y, W, 24, 0, g_head);
    y += 30;
    g_launch = make(L"BUTTON", L"&Lancer le jeu", BS_DEFPUSHBUTTON | WS_TABSTOP, L, y, 260, 42, ID_LAUNCH, g_bold);
    make(L"BUTTON", L"&Retirer le mod", BS_PUSHBUTTON | WS_TABSTOP, L + 272, y, 150, 42, ID_REMOVE);
    y += 54;
    g_state_rect = {px(L), px(y), px(L + W), px(y + 52)};
    g_state = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, L + 14, y + 8, W - 22, 40, 0, g_bold);
    set_state(L"Choisis ton rôle puis lance le jeu.", kMuted);
    y += 64;

    make(L"STATIC", L"Joueurs", 0, L, y, W, 24, 0, g_head);
    y += 28;
    g_players = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, L, y, W, 104,
                     ID_PLAYERS);
    ListView_SetExtendedListViewStyle(g_players, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t *cols[] = {L"Joueur", L"Zone", L"État", L"Connexion"};
    const int widths[] = {190, 150, 90, 110};
    for (int c = 0; c < 4; ++c) {
        LVCOLUMNW col = {LVCF_TEXT | LVCF_WIDTH};
        col.pszText = const_cast<wchar_t *>(cols[c]);
        col.cx = px(widths[c]);
        ListView_InsertColumn(g_players, c, &col);
    }
    y += 112;

    make(L"STATIC", L"Événements", 0, L, y, W, 24, 0, g_head);
    y += 28;
    g_events = make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, L, y, W, 96,
                    ID_EVENTS);
    y += 104;
    make(L"BUTTON", L"Copier mes s&auvegardes", BS_PUSHBUTTON | WS_TABSTOP, L, y, 200, 32, ID_BACKUP);
    make(L"BUTTON", L"E&xporter le journal", BS_PUSHBUTTON | WS_TABSTOP, L + 210, y, 180, 32, ID_EXPORT);
    y += 42;
    make(L"STATIC",
         L"Invité : attends « Connecté ! » avant Start. LB + RB + Y (1,5 s) ou F7 : explorer seul / revenir. Raccourcis : "
         L"Alt + la lettre soulignée, Tab pour passer d'un élément à l'autre.",
         0, L, y, W, 40, 0);
    y += 52;
    g_height = y;

    std::wstring role = setting(L"role", L"host");
    SendMessageW(role == L"guest" ? g_guest : role == L"solo" ? g_solo : g_host, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_keep, BM_SETCHECK, setting(L"keep_progress", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_own_char, BM_SETCHECK, setting(L"own_character", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_debug, BM_SETCHECK, setting(L"debug_log", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    update_players();
}

void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    HBRUSH accent = CreateSolidBrush(kAccent);
    FillRect(dc, &g_header_rect, accent);
    DeleteObject(accent);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, g_title);
    RECT t = {px(kL), px(8), px(kW - kL), px(40)};
    DrawTextW(dc, L"DDDA Coop", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(245, 225, 225));
    RECT sub = {px(kL), px(38), px(kW - kL), px(60)};
    DrawTextW(dc, L"Dragon's Dogma: Dark Arisen à plusieurs, chacun sur son PC", -1, &sub, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    // the state card: white, with a coloured bar on the left
    FillRect(dc, &g_state_rect, g_card_brush);
    RECT bar = {g_state_rect.left, g_state_rect.top, g_state_rect.left + px(6), g_state_rect.bottom};
    HBRUSH b = CreateSolidBrush(g_state_color);
    FillRect(dc, &bar, b);
    DeleteObject(b);
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
        return 0;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE: on_browse(); break;
        case ID_HOST:
        case ID_GUEST:
        case ID_SOLO:
        case ID_KEEP:
        case ID_OWN_CHAR:
        case ID_DEBUG: update_controls(); save_settings(); break;
        case ID_LAUNCH: on_launch(); break;
        case ID_REMOVE: on_remove(); break;
        case ID_BACKUP: on_backup(); break;
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
        poll_log();
        check_watchers();
        update_players();
        return 0;
    case WM_CTLCOLORSTATIC: // labels, radio buttons and check boxes on the light background
        if (HWND(l) == g_state) {
            SetTextColor(HDC(w), g_state_color);
            SetBkColor(HDC(w), kCard);
            return LRESULT(g_card_brush);
        }
        SetTextColor(HDC(w), HWND(l) == g_version ? kMuted : kText);
        SetBkColor(HDC(w), kBg);
        return LRESULT(g_bg_brush);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
    }
    LocalFree(argv);
    g_steam_dir = steam_arg.empty() ? steam_dir() : steam_arg;
    g_game_dir = !game_arg.empty() ? game_arg : setting(L"game", L"");
    if (g_game_dir.empty())
        g_game_dir = find_game(g_steam_dir);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    // Segoe UI 10 pt (a little larger than the system's 9 pt), headings 12 pt, title 18 pt
    auto font = [](int pt, int weight) {
        LOGFONTW f = {};
        f.lfHeight = -MulDiv(pt, g_dpi, 72);
        f.lfWeight = weight;
        f.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(f.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&f);
    };
    g_font = font(10, FW_NORMAL);
    g_bold = font(10, FW_SEMIBOLD);
    g_head = font(12, FW_SEMIBOLD);
    g_title = font(18, FW_BOLD);
    g_bg_brush = CreateSolidBrush(kBg);
    g_card_brush = CreateSolidBrush(kCard);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = L"DDDACoopLauncher";
    RegisterClassW(&wc);
    RECT r = {0, 0, MulDiv(600, g_dpi, 96), MulDiv(900, g_dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT wr;
    GetWindowRect(h, &wr);
    RECT need = {0, 0, MulDiv(600, g_dpi, 96), MulDiv(g_height, g_dpi, 96)};
    AdjustWindowRect(&need, style, FALSE);
    SetWindowPos(h, nullptr, 0, 0, need.right - need.left, need.bottom - need.top, SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(h, show);
    check_game();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(h, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
