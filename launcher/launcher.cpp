// DDDA Coop: a small window to run beside the game. It finds the Steam install of Dragon's Dogma:
// Dark Arisen, checks the DDDA.exe build, copies the player's saves to Documents, installs
// dinput8.dll and writes coop.ini for the chosen role, starts the game through Steam, and shows
// the co-op state read from the game's coop.log. "Retirer le mod" deletes dinput8.dll and coop.ini.
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
    ID_BACKUP, ID_EXPORT, ID_EVENTS, ID_SOLO, ID_KEEP, ID_TIMER
};

HWND g_wnd, g_game_edit, g_version, g_host, g_guest, g_solo, g_own_char, g_keep, g_debug, g_launch, g_state, g_events;
HFONT g_font, g_bold;
std::wstring g_exe_dir, g_game_dir, g_steam_dir, g_docs_dir;
bool g_no_launch = false;
bool g_supported = false;
COLORREF g_state_color = RGB(80, 80, 80);
FILETIME g_launch_time = {};
long long g_log_pos = 0;
bool g_watching = false;
int g_friends = 0;
bool g_world_received = false;

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
    SetWindowTextW(g_state, text.c_str());
    InvalidateRect(g_state, nullptr, TRUE);
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
    g_log_pos = 0;
    g_watching = true;
    g_friends = 0;
    g_world_received = false;
    if (!g_no_launch)
        ShellExecuteW(g_wnd, L"open", L"steam://rungameid/367500", nullptr, nullptr, SW_SHOWNORMAL);
    add_event(L"Lancement du jeu par Steam…");
    set_state(guest ? L"Lancement… Reste sur l'écran titre jusqu'à ce que l'hôte soit trouvé."
                    : L"Lancement… Charge ta partie normalement.",
              RGB(80, 80, 80));
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
    set_state(ok ? L"Mod retiré." : L"Erreur en retirant le mod.", ok ? RGB(80, 80, 80) : RGB(190, 30, 30));
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
    if (has("unsupported DDDA.exe")) {
        add_event(L"Version du jeu non prise en charge : le mod s'est désactivé.");
        set_state(L"Mod désactivé : version du jeu non prise en charge.", RGB(190, 30, 30));
    } else if (has("waiting for Steam")) {
        set_state(L"En attente de Steam…", RGB(80, 80, 80));
    } else if (has("steam: link up")) {
        add_event(L"Connecté à Steam.");
        set_state(guest ? L"Recherche de l'hôte parmi tes amis Steam… Reste sur l'écran titre."
                        : L"Prêt : en attente de tes amis. Tu peux jouer.",
                  RGB(80, 80, 80));
    } else if (has("steam: host found")) {
        add_event(L"Hôte trouvé, réception de son monde…");
        set_state(L"Hôte trouvé. Réception de son monde… Reste sur l'écran titre.", RGB(80, 80, 80));
    } else if (has("net: save: the host's save version") && has("arrived")) {
        if (!g_world_received)
            set_state(L"Connecté ! Appuie sur Start, puis choisis « Charger ».", RGB(20, 140, 40));
        add_event(g_world_received ? L"Monde de l'hôte mis à jour." : L"Monde de l'hôte reçu.");
        g_world_received = true;
    } else if (has("net: guest connected from")) {
        int k = number_after(l, "(guest ");
        ++g_friends;
        add_event(L"Ami " + std::to_wstring(k) + L" connecté.");
        set_state(std::to_wstring(g_friends) + (g_friends > 1 ? L" amis connectés." : L" ami connecté."), RGB(20, 140, 40));
    } else if (has("net: guest ") && has(", dropped")) {
        int k = number_after(l, "net: guest ");
        g_friends = g_friends > 0 ? g_friends - 1 : 0;
        add_event(L"Ami " + std::to_wstring(k) + L" déconnecté (son pion repasse à l'IA).");
        set_state(g_friends ? std::to_wstring(g_friends) + (g_friends > 1 ? L" amis connectés." : L" ami connecté.")
                            : L"En attente de tes amis. Tu peux jouer.",
                  g_friends ? RGB(20, 140, 40) : RGB(80, 80, 80));
    } else if (has("'s character arrived")) {
        int k = number_after(l, "net: guest ");
        add_event(L"Le perso de l'ami " + std::to_wstring(k) + L" est arrivé : il remplace un pion.");
    } else if (has("rejoin: idle -> saving")) {
        add_event(L"Sauvegarde et rechargement pour faire entrer un ami (quelques secondes)…");
    } else if (has("rejoin: world reloaded")) {
        add_event(L"Monde rechargé.");
    } else if (has("net: peer silent")) {
        add_event(L"Connexion à l'hôte perdue : reconnexion automatique…");
        set_state(L"Connexion à l'hôte perdue, reconnexion…", RGB(200, 120, 0));
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
        set_state(L"Exploration libre : ce que tu fais ne compte que chez toi (sauf ta progression).", RGB(20, 100, 160));
    } else if (has("roam: back with the host")) {
        add_event(L"De retour avec l'hôte.");
        set_state(L"Connecté : tu joues avec l'hôte.", RGB(20, 140, 40));
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

// ---- window ----

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(font ? font : g_font), TRUE);
    return c;
}

void build_window()
{
    const int L = 16, W = 520;
    int y = 14;
    make(L"STATIC", L"Dossier du jeu", 0, L, y, W, 20, 0, g_bold);
    y += 24;
    g_game_edit = make(L"EDIT", g_game_dir.c_str(), WS_BORDER | ES_AUTOHSCROLL, L, y, W - 100, 24, ID_GAME_EDIT);
    make(L"BUTTON", L"Changer…", BS_PUSHBUTTON, L + W - 92, y, 92, 24, ID_BROWSE);
    y += 28;
    g_version = make(L"STATIC", L"", 0, L, y, W, 20, 0);
    y += 32;

    make(L"STATIC", L"Je suis", 0, L, y, W, 20, 0, g_bold);
    y += 24;
    g_host = make(L"BUTTON", L"L'hôte : je charge ma partie, mes amis me rejoignent", BS_AUTORADIOBUTTON | WS_GROUP,
                  L, y, W, 22, ID_HOST);
    y += 24;
    g_guest = make(L"BUTTON", L"Un invité : je rejoins un ami (ma partie n'est ni chargée ni sauvegardée)",
                   BS_AUTORADIOBUTTON, L, y, W, 22, ID_GUEST);
    y += 24;
    g_own_char = make(L"BUTTON", L"Jouer avec mon propre perso (sinon, je joue le pion de l'hôte)", BS_AUTOCHECKBOX,
                      L + 20, y, W - 20, 22, ID_OWN_CHAR);
    y += 24;
    g_solo = make(L"BUTTON", L"Seul : je joue ma partie (ma progression gagnée en co-op y est ajoutée)",
                  BS_AUTORADIOBUTTON, L, y, W, 22, ID_SOLO);
    y += 30;
    g_keep = make(L"BUTTON", L"Garder la progression de mon perso gagnée chez un ami (exp, discipline, objets)",
                  BS_AUTOCHECKBOX, L, y, W, 22, ID_KEEP);
    y += 24;
    g_debug = make(L"BUTTON", L"Journal détaillé (utile pour signaler un problème)", BS_AUTOCHECKBOX, L, y, W, 22, ID_DEBUG);
    y += 34;

    g_launch = make(L"BUTTON", L"Lancer le jeu", BS_DEFPUSHBUTTON, L, y, 250, 36, ID_LAUNCH, g_bold);
    make(L"BUTTON", L"Retirer le mod", BS_PUSHBUTTON, L + 262, y, 120, 36, ID_REMOVE);
    y += 50;

    make(L"STATIC", L"État", 0, L, y, W, 20, 0, g_bold);
    y += 22;
    g_state = make(L"STATIC", L"Choisis ton rôle puis lance le jeu.", 0, L, y, W, 40, 0, g_bold);
    y += 44;
    g_events = make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, L, y, W, 150, ID_EVENTS);
    y += 158;
    make(L"BUTTON", L"Copier mes sauvegardes", BS_PUSHBUTTON, L, y, 180, 28, ID_BACKUP);
    make(L"BUTTON", L"Exporter le journal", BS_PUSHBUTTON, L + 190, y, 160, 28, ID_EXPORT);
    y += 40;
    make(L"STATIC",
         L"Invité : attends « Connecté ! » avant Start. LB + RB + Y (1,5 s) ou F7 : explorer seul / revenir. "
         L"Pour ta propre partie : « Seul » (ta progression co-op y est ajoutée) ou « Retirer le mod ».",
         0, L, y, W, 36, 0);

    std::wstring role = setting(L"role", L"host");
    SendMessageW(role == L"guest" ? g_guest : role == L"solo" ? g_solo : g_host, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_keep, BM_SETCHECK, setting(L"keep_progress", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_own_char, BM_SETCHECK, setting(L"own_character", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_debug, BM_SETCHECK, setting(L"debug_log", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        build_window();
        SetTimer(h, ID_TIMER, 1000, nullptr);
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
        return 0;
    case WM_CTLCOLORSTATIC:
        if (HWND(l) == g_state) {
            SetTextColor(HDC(w), g_state_color);
            SetBkColor(HDC(w), GetSysColor(COLOR_BTNFACE));
            return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
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
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
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

    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&bold);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"DDDACoopLauncher";
    RegisterClassW(&wc);
    RECT r = {0, 0, 552, 664};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
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
