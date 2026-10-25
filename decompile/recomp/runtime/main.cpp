// Point d'entrée de pikmin3.exe : prépare la mémoire invitée, charge l'image du jeu
// (carrot_linked.elf, produit par scripts/link_rpl.py), puis exécute le point d'entrée
// recompilé du RPX sur le thread principal Cafe OS.
#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace rt;

namespace rt
{
extern fs::path g_gameDir, g_updateDir, g_dlcDir, g_saveDir;
void memHeapsInit(uint32_t mem2Start);
void decafGx2Register();
void decafSndRegister();
extern bool g_noSound;
extern std::string g_recordInput, g_replayInput;
void decafGx2RunEntry();
void gpuStart();
void gpuMessageLoop(void* gameThread);
extern bool g_captureWindow;
extern bool g_splitView;
extern bool g_probe;
extern uint32_t g_autoA;
extern uint32_t g_rdocCaptureAt;
extern uint32_t g_profileSeconds;
void profileStart();
}

namespace
{

struct LoadedImage
{
    uint32_t entry = 0;
    uint32_t dataEnd = 0;
    uint32_t sdaBase = 0;
    uint32_t sda2Base = 0;
    uint32_t stackSize = 0x40000;
};

template <typename T>
T be(const std::vector<uint8_t>& d, size_t off)
{
    T v;
    memcpy(&v, d.data() + off, sizeof(T));
    if constexpr (sizeof(T) == 2)
        return T(__builtin_bswap16(v));
    else
        return T(__builtin_bswap32(v));
}

LoadedImage loadImage(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        fatal("image introuvable : %s (lancer scripts/link_rpl.py)", pathToUtf8(path).c_str());
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 0x34 || memcmp(d.data(), "\x7F" "ELF", 4) != 0)
        fatal("%s n'est pas un ELF", pathToUtf8(path).c_str());

    LoadedImage img;
    img.entry = be<uint32_t>(d, 0x18);
    const uint32_t shoff = be<uint32_t>(d, 0x20);
    const uint16_t shentsize = be<uint16_t>(d, 0x2E), shnum = be<uint16_t>(d, 0x30);
    for (uint32_t i = 0; i < shnum; i++)
    {
        const size_t h = shoff + i * shentsize;
        const uint32_t type = be<uint32_t>(d, h + 4), flags = be<uint32_t>(d, h + 8);
        const uint32_t addr = be<uint32_t>(d, h + 12), off = be<uint32_t>(d, h + 16), size = be<uint32_t>(d, h + 20);
        if (type == 0x80000004) // SHT_RPL_FILEINFO
        {
            img.sdaBase = be<uint32_t>(d, off + 0x24);
            img.sda2Base = be<uint32_t>(d, off + 0x28);
            img.stackSize = be<uint32_t>(d, off + 0x2C);
            continue;
        }
        if (!(flags & 2) || !addr || !size) // SHF_ALLOC
            continue;
        if (type == 8) // SHT_NOBITS (.bss)
            memset(ptr(addr), 0, size);
        else
            memcpy(ptr(addr), d.data() + off, size);
        if (addr >= kMem2Base)
            img.dataEnd = std::max(img.dataEnd, addr + size);
    }
    RT_INFO("image chargée : entrée 0x%08X, fin des données 0x%08X, SDA 0x%08X / 0x%08X", img.entry,
        img.dataEnd, img.sdaBase, img.sda2Base);
    return img;
}

// Cherche les dossiers « PIKMIN 3 [Game|Update|DLC] » à partir de `root` et de ses parents.
bool findGameDirs(fs::path root)
{
    for (; !root.empty(); root = root.parent_path())
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(root, ec))
        {
            const std::string name = pathToUtf8(e.path().filename());
            if (!e.is_directory())
                continue;
            if (name.find("[Game]") != std::string::npos)
                g_gameDir = e.path();
            else if (name.find("[Update]") != std::string::npos)
                g_updateDir = e.path();
            else if (name.find("[DLC]") != std::string::npos)
                g_dlcDir = e.path();
        }
        if (!g_gameDir.empty())
            return true;
        if (root == root.parent_path())
            break;
    }
    return false;
}

fs::path exeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

LoadedImage g_image;

DWORD WINAPI gameThread(void*)
{
    kernel::Thread* t = kernel::createMainThread(std::max<uint32_t>(g_image.stackSize, 0x100000));
    PPCContext& ctx = *t->ctx;
    // argc = 1, argv = { "carrot.rpx", NULL }
    const uint32_t argv = sysAlloc(16);
    const uint32_t arg0 = sysAlloc(16);
    writeString(arg0, "carrot.rpx");
    store32(argv, arg0);
    ctx.r3.u64 = 1;
    ctx.r4.u64 = argv;
    decafGx2RunEntry(); // initialisation de gx2.rpl (rpl_entry), comme au chargement du module
    RT_INFO("lancement du point d'entrée 0x%08X", g_image.entry);
    uint32_t code = 0;
    try
    {
        PPCCallIndirect(ctx, g_base, g_image.entry);
        code = ctx.r3.u32;
    }
    catch (const kernel::ThreadExit& e)
    {
        code = e.value;
    }
    RT_INFO("le point d'entrée a rendu la main (code %d)", int32_t(code));
    return code;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    // Minuteur système à 1 ms : les attentes (alarmes dont la vsync à 60 Hz, OSSleepTicks…) se
    // réveillent sinon par pas de 15,6 ms, et la vsync arrivait jusqu'à 15 ms en retard.
    timeBeginPeriod(1);
    fs::path root = exeDir();
    fs::path imagePath = fs::path(PIKMIN3_RECOMP_DIR) / "carrot_linked.elf";
    int dumpAfter = 0;
    bool inputTest = false;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--trace")
            g_traceHle = true;
        else if (a == "--trace-filter" && i + 1 < argc)
        {
            g_traceHle = true;
            g_traceFilter = argv[++i];
        }
        else if (a == "--record" && i + 1 < argc)
        {
            g_recordHle = true;
            g_recordFilter = argv[++i];
            if (g_recordFilter == "*")
                g_recordFilter.clear();
        }
        else if (a == "--capture")
            g_captureWindow = true;
        else if (a == "--split")
            g_splitView = true;
        else if (a == "--probe")
            g_probe = true;
        else if (a == "--profile" && i + 1 < argc)
            g_profileSeconds = uint32_t(atoi(argv[++i]));
        else if (a == "--rdoc-capture" && i + 1 < argc)
            g_rdocCaptureAt = uint32_t(atoi(argv[++i]));
        else if (a == "--record-input" && i + 1 < argc)
            g_recordInput = argv[++i];
        else if (a == "--replay-input" && i + 1 < argc)
            g_replayInput = argv[++i];
        else if (a == "--no-sound")
            g_noSound = true;
        else if (a == "--auto-a" && i + 1 < argc)
            g_autoA = uint32_t(atoi(argv[++i]));
        else if (a == "--input-test")
            inputTest = true;
        else if (a == "--dump" && i + 1 < argc)
            dumpAfter = atoi(argv[++i]);
        else if (a == "--root" && i + 1 < argc)
            root = fs::u8path(argv[++i]);
        else if (a == "--image" && i + 1 < argc)
            imagePath = fs::u8path(argv[++i]);
        else
        {
            fprintf(stderr, "usage : pikmin3 [--root <dossier des données du jeu>] [--image carrot_linked.elf] [--trace] [--dump <secondes>] [--input-test] [--auto-a <ms>]\n");
            return 2;
        }
    }

    if (!findGameDirs(root) && !findGameDirs(fs::path(PIKMIN3_RECOMP_DIR)))
        fatal("dossier « PIKMIN 3 [Game] … » introuvable (option --root)");
    g_saveDir = exeDir() / "save";
    // Sans --record-input ni --replay-input, chaque partie est enregistrée (commandes, tactile)
    // dans build/inputs/partie_<date>_<heure>.txt, rejouable avec --replay-input.
    if (g_recordInput.empty() && g_replayInput.empty())
    {
        const fs::path dir = exeDir() / "inputs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const time_t now = time(nullptr);
        char name[64];
        strftime(name, sizeof(name), "partie_%Y%m%d_%H%M%S.txt", localtime(&now));
        g_recordInput = pathToUtf8(dir / name);
        RT_INFO("enregistrement des commandes : %s", g_recordInput.c_str());
    }
    RT_INFO("jeu : %s", pathToUtf8(g_gameDir).c_str());
    RT_INFO("update : %s", pathToUtf8(g_updateDir).c_str());
    RT_INFO("DLC : %s", pathToUtf8(g_dlcDir).c_str());

    memInit();
    cpuInit();
    g_image = loadImage(imagePath);
    kernel::setSdaBases(g_image.sdaBase, g_image.sda2Base);
    memHeapsInit((g_image.dataEnd + 0xFFFF) & ~0xFFFFu);
    hleInit();
    // DECAF_WATCH_ADDR=<adresse hex>[,taille] : journalise les écritures dès le démarrage (watch.cpp)
    if (const char* watch = getenv("DECAF_WATCH_ADDR"))
    {
        const char* comma = strchr(watch, ',');
        watchRange(uint32_t(strtoul(watch, nullptr, 16)), comma ? uint32_t(strtoul(comma + 1, nullptr, 0)) : 4, 20, "DECAF_WATCH_ADDR");
    }
    decafGx2Register();
    decafSndRegister();
    gpuStart();
    profileStart();

    if (dumpAfter > 0)
        std::thread([dumpAfter] {
            std::this_thread::sleep_for(std::chrono::seconds(dumpAfter));
            kernel::dumpThreads();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            kernel::dumpThreads();
            if (g_recordHle)
                dumpHleEvents(1 << 14);
        }).detach();

    if (inputTest)
    {
        // Mode de test des commandes : fenêtre seule, sans lancer le jeu. Chaque changement
        // de boutons ou de sticks est journalisé (console et pikmin3.log) par VPADRead.
        RT_INFO("test des commandes : appuie sur des touches avec la fenêtre au premier plan ; fermer la fenêtre pour quitter");
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
            PPCContext ctx {};
            const uint32_t buf = sysAlloc(0xB0), err = sysAlloc(4);
            for (;;)
            {
                ctx.r3.u64 = 0;
                ctx.r4.u64 = buf;
                ctx.r5.u64 = 1;
                ctx.r6.u64 = err;
                hleInvoke("vpad", "VPADRead", ctx);
                Sleep(16);
            }
        }, nullptr, 0, nullptr);
        gpuMessageLoop(t);
        return 0;
    }

    HANDLE h = CreateThread(nullptr, 256u << 20, gameThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    gpuMessageLoop(h);
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    return int(code);
}
