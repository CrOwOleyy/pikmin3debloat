// coreinit FS* et nn_save SAVE* : accès aux fichiers du jeu sur le disque hôte.
//
//   /vol/content/...      -> <Update>/content puis <Game>/content (l'update prime)
//   /vol/aoc<titre>/...   -> <DLC>/content
//   /vol/save/...         -> <dossier de sauvegarde>
#include "kernel.h"

#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace rt;

namespace rt
{
fs::path g_gameDir, g_updateDir, g_dlcDir, g_saveDir;
}

namespace
{

constexpr int32_t kStatusOk = 0;
constexpr int32_t kStatusEnd = -2;
constexpr int32_t kStatusNotFound = -6;
constexpr int32_t kStatusNotFile = -7;
constexpr int32_t kStatusPermission = -10;

struct DirState
{
    std::vector<fs::directory_entry> entries;
    size_t next = 0;
};

std::mutex s_fsMutex;
std::unordered_map<uint32_t, FILE*> s_files;
std::unordered_map<uint32_t, DirState> s_dirs;
uint32_t s_nextHandle = 1;
std::string s_cwd = "/"; // répertoire courant initial d'un client FS (decaf : "/")

std::string normalize(std::string p)
{
    if (!p.empty() && p[0] != '/')
        p = s_cwd + p;
    std::string out;
    for (size_t i = 0; i < p.size(); i++)
        if (!(p[i] == '/' && !out.empty() && out.back() == '/'))
            out += p[i];
    return out;
}

// Résout un chemin Cafe en chemin hôte ; `forWrite` : pas de repli sur le contenu de base.
fs::path resolve(const std::string& guestPath, bool forWrite = false)
{
    const std::string p = normalize(guestPath);
    auto rel = [&](size_t n) { return fs::u8path(p.substr(n)); };
    if (p.rfind("/vol/content/", 0) == 0 || p == "/vol/content")
    {
        const fs::path r = p.size() > 13 ? rel(13) : fs::path();
        // Surcharges locales (fichiers manquants ou abîmés dans le dump), prioritaires.
        if (!forWrite && !r.empty())
        {
            const fs::path over = fs::path(PIKMIN3_RECOMP_DIR) / "content_override" / r;
            if (fs::exists(over))
                return over;
        }
        const fs::path upd = g_updateDir / "content" / r;
        if (forWrite || fs::exists(upd))
            return upd;
        return g_gameDir / "content" / r;
    }
    if (p.rfind("/vol/aoc", 0) == 0)
    {
        const size_t slash = p.find('/', 5 + 3);
        return g_dlcDir / "content" / (slash == std::string::npos ? fs::path() : rel(slash + 1));
    }
    if (p.rfind("/vol/save/", 0) == 0)
        return g_saveDir / rel(10);
    if (p.rfind("/vol/meta/", 0) == 0)
        return g_gameDir / "meta" / rel(10);
    return {};
}

void fillStat(uint32_t stat, const fs::path& path)
{
    memset(ptr(stat), 0, 0x64);
    std::error_code ec;
    const bool dir = fs::is_directory(path, ec);
    store32(stat + 0x00, dir ? 0x80000000 : 0x01000000);
    store32(stat + 0x04, 0x666);
    store32(stat + 0x10, dir ? 0 : uint32_t(fs::file_size(path, ec)));
}

int32_t openFile(const std::string& guestPath, const std::string& mode, uint32_t outHandle)
{
    const bool write = mode.find_first_of("wa+") != std::string::npos;
    const fs::path host = resolve(guestPath, write);
    if (host.empty())
    {
        RT_WARN("FSOpenFile : chemin non géré %s", guestPath.c_str());
        return kStatusNotFound;
    }
    if (write)
        fs::create_directories(host.parent_path());
    std::string m = mode;
    if (m.find('b') == std::string::npos)
        m += 'b';
    FILE* f = _wfopen(host.c_str(), std::wstring(m.begin(), m.end()).c_str());
    if (!f)
    {
        RT_TRACE("FSOpenFile : introuvable %s", guestPath.c_str());
        return fs::is_directory(host) ? kStatusNotFile : kStatusNotFound;
    }
    std::lock_guard lock(s_fsMutex);
    const uint32_t h = s_nextHandle++;
    s_files[h] = f;
    store32(outHandle, h);
    RT_INFO("ouverture %s", guestPath.c_str());
    return kStatusOk;
}

FILE* fileOf(uint32_t h)
{
    std::lock_guard lock(s_fsMutex);
    auto it = s_files.find(h);
    return it == s_files.end() ? nullptr : it->second;
}

int32_t openDir(const std::string& guestPath, uint32_t outHandle)
{
    const fs::path host = resolve(guestPath);
    std::error_code ec;
    if (host.empty() || !fs::is_directory(host, ec))
        return kStatusNotFound;
    DirState d;
    for (auto& e : fs::directory_iterator(host, ec))
        d.entries.push_back(e);
    // Contenu : union update + base
    if (normalize(guestPath).rfind("/vol/content", 0) == 0)
    {
        const fs::path base = g_gameDir / "content" / fs::relative(host, g_updateDir / "content", ec);
        if (host != base && fs::is_directory(base, ec))
            for (auto& e : fs::directory_iterator(base, ec))
            {
                bool dup = false;
                for (auto& x : d.entries)
                    dup |= x.path().filename() == e.path().filename();
                if (!dup)
                    d.entries.push_back(e);
            }
    }
    std::lock_guard lock(s_fsMutex);
    const uint32_t h = s_nextHandle++;
    s_dirs[h] = std::move(d);
    store32(outHandle, h);
    return kStatusOk;
}

int32_t readDir(uint32_t h, uint32_t entry)
{
    std::lock_guard lock(s_fsMutex);
    auto it = s_dirs.find(h);
    if (it == s_dirs.end() || it->second.next >= it->second.entries.size())
        return kStatusEnd;
    const auto& e = it->second.entries[it->second.next++];
    fillStat(entry, e.path());
    const std::string name = pathToUtf8(e.path().filename());
    memset(ptr(entry + 0x64), 0, 256);
    memcpy(ptr(entry + 0x64), name.c_str(), std::min<size_t>(name.size(), 255));
    return kStatusOk;
}

} // namespace

HLE_FUNC(coreinit, FSInit) {}
HLE_FUNC(coreinit, FSShutdown) {}
HLE_FUNC(coreinit, FSAddClient) { RET_S32(kStatusOk); }
HLE_FUNC(coreinit, FSDelClient) { RET_S32(kStatusOk); }
HLE_FUNC(coreinit, FSInitCmdBlock) { memset(ptr(ARG_U32(3)), 0, 0xA80); }
HLE_FUNC(coreinit, FSSetCmdPriority) { RET_S32(kStatusOk); }
HLE_FUNC(coreinit, FSSetStateChangeNotification) {}
HLE_FUNC(coreinit, FSGetVolumeState) { RET_U32(1); }
HLE_FUNC(coreinit, FSGetLastError) { RET_S32(0); }
HLE_FUNC(coreinit, FSGetLastErrorCodeForViewer) { RET_S32(0); }

// FSStatus FSGetCwd(client, block, char* out, uint32_t bytes, errorMask)
HLE_FUNC(coreinit, FSGetCwd)
{
    const uint32_t out = ARG_U32(5), size = ARG_U32(6);
    if (out && size)
    {
        const std::string c = s_cwd.substr(0, size - 1);
        writeString(out, c);
    }
    RET_S32(kStatusOk);
}

// FSStatus FSOpenFile(client, block, path, mode, FSFileHandle* out, errorMask)
HLE_FUNC(coreinit, FSOpenFile)
{
    RET_S32(openFile(readString(ARG_U32(5)), readString(ARG_U32(6)), ARG_U32(7)));
}

HLE_FUNC(coreinit, FSCloseFile)
{
    std::lock_guard lock(s_fsMutex);
    if (auto it = s_files.find(ARG_U32(5)); it != s_files.end())
    {
        fclose(it->second);
        s_files.erase(it);
    }
    RET_S32(kStatusOk);
}

// int32 FSReadFile(client, block, buffer, size, count, handle, flag, errorMask) -> éléments lus
HLE_FUNC(coreinit, FSReadFile)
{
    FILE* f = fileOf(ARG_U32(8));
    if (!f)
    {
        RET_S32(kStatusNotFound);
        return;
    }
    const uint32_t size = ARG_U32(6), count = ARG_U32(7);
    const size_t bytes = fread(ptr(ARG_U32(5)), 1, size_t(size) * count, f);
    RET_S32(size ? int32_t(bytes / size) : 0);
}

HLE_FUNC(coreinit, FSWriteFile)
{
    FILE* f = fileOf(ARG_U32(8));
    if (!f)
    {
        RET_S32(kStatusNotFound);
        return;
    }
    const uint32_t size = ARG_U32(6), count = ARG_U32(7);
    const size_t bytes = fwrite(ptr(ARG_U32(5)), 1, size_t(size) * count, f);
    RET_S32(size ? int32_t(bytes / size) : 0);
}

// FSStatus FSSetPosFile(client, block, handle, pos, errorMask)
HLE_FUNC(coreinit, FSSetPosFile)
{
    FILE* f = fileOf(ARG_U32(5));
    RET_S32(f && _fseeki64(f, ARG_U32(6), SEEK_SET) == 0 ? kStatusOk : kStatusNotFound);
}

// FSStatus FSGetStatFile(client, block, handle, FSStat* out, errorMask)
HLE_FUNC(coreinit, FSGetStatFile)
{
    FILE* f = fileOf(ARG_U32(5));
    if (!f)
    {
        RET_S32(kStatusNotFound);
        return;
    }
    const uint32_t stat = ARG_U32(6);
    memset(ptr(stat), 0, 0x64);
    const int64_t pos = _ftelli64(f);
    _fseeki64(f, 0, SEEK_END);
    store32(stat + 0x00, 0x01000000);
    store32(stat + 0x04, 0x666);
    store32(stat + 0x10, uint32_t(_ftelli64(f)));
    _fseeki64(f, pos, SEEK_SET);
    RET_S32(kStatusOk);
}

// FSStatus FSGetStat(client, block, path, FSStat* out, errorMask)
HLE_FUNC(coreinit, FSGetStat)
{
    const fs::path host = resolve(readString(ARG_U32(5)));
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec))
    {
        RET_S32(kStatusNotFound);
        return;
    }
    fillStat(ARG_U32(6), host);
    RET_S32(kStatusOk);
}

HLE_FUNC(coreinit, FSOpenDir) { RET_S32(openDir(readString(ARG_U32(5)), ARG_U32(6))); }
HLE_FUNC(coreinit, FSReadDir) { RET_S32(readDir(ARG_U32(5), ARG_U32(6))); }
HLE_FUNC(coreinit, FSCloseDir)
{
    std::lock_guard lock(s_fsMutex);
    s_dirs.erase(ARG_U32(5));
    RET_S32(kStatusOk);
}

// ================================================================= nn_save
// Les sauvegardes d'un compte vivent dans /vol/save/<compte 8 chiffres hex>/...
static std::string savePath(uint32_t account, uint32_t path)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "/vol/save/%08X/", account == 0xFF ? 0x80000001u : account);
    std::string rel = readString(path);
    while (!rel.empty() && rel[0] == '/')
        rel.erase(0, 1);
    return buf + rel;
}

HLE_FUNC(nn_save, SAVEInit) { RET_S32(0); }
HLE_FUNC(nn_save, SAVEShutdown) {}
HLE_FUNC(nn_save, SAVEInitSaveDir)
{
    fs::create_directories(resolve(savePath(ARG_U32(3) & 0xFF, 0), true));
    RET_S32(0);
}
HLE_FUNC(nn_save, SAVEFlushQuota) { RET_S32(0); }

// SAVEOpenFile(client, block, account, path, mode, FSFileHandle* out, errorMask)
HLE_FUNC(nn_save, SAVEOpenFile)
{
    RET_S32(openFile(savePath(ARG_U32(5) & 0xFF, ARG_U32(6)), readString(ARG_U32(7)), ARG_U32(8)));
}
// SAVEOpenFileOtherApplication(client, block, titleId(u64 r5:r6), account, path, mode, out, errMask)
HLE_FUNC(nn_save, SAVEOpenFileOtherApplication)
{
    RET_S32(openFile(savePath(ARG_U32(7) & 0xFF, ARG_U32(8)), readString(ARG_U32(9)), ARG_U32(10)));
}

// SAVEOpenDir(client, block, account, path, FSDirHandle* out, errorMask)
HLE_FUNC(nn_save, SAVEOpenDir) { RET_S32(openDir(savePath(ARG_U32(5) & 0xFF, ARG_U32(6)), ARG_U32(7))); }
HLE_FUNC(nn_save, SAVEOpenDirOtherApplication) { RET_S32(kStatusNotFound); }

// SAVEGetStat(client, block, account, path, FSStat* out, errorMask)
HLE_FUNC(nn_save, SAVEGetStat)
{
    const fs::path host = resolve(savePath(ARG_U32(5) & 0xFF, ARG_U32(6)));
    std::error_code ec;
    if (!fs::exists(host, ec))
    {
        RET_S32(kStatusNotFound);
        return;
    }
    fillStat(ARG_U32(7), host);
    RET_S32(kStatusOk);
}
HLE_FUNC(nn_save, SAVEGetStatOtherApplication) { RET_S32(kStatusNotFound); }

// SAVERemove(client, block, account, path, errorMask)
HLE_FUNC(nn_save, SAVERemove)
{
    std::error_code ec;
    fs::remove_all(resolve(savePath(ARG_U32(5) & 0xFF, ARG_U32(6)), true), ec);
    RET_S32(kStatusOk);
}
