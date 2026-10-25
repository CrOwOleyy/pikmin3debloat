// Caméra libre (F9), et recherche des caméras du jeu en mémoire (DECAF_CAM_SCAN). Une caméra
// sead (LookAtCamera) range une matrice de vue 3x4 suivie de la position, du point visé et du
// vecteur haut ; on cherche une matrice dont les trois lignes sont unitaires et orthogonales, et
// qui envoie la position qui la suit sur l'origine.
#include "runtime.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace rt
{

namespace
{
}
float loadFloatBE(const uint8_t* base, uint64_t a);
namespace
{
float loadFloat(const uint8_t* base, uint64_t a)
{
    uint32_t v;
    memcpy(&v, base + a, 4);
    v = __builtin_bswap32(v);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

}
float loadFloatBE(const uint8_t* base, uint64_t a) { return loadFloat(base, a); }
namespace
{
bool closeTo(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}
}

// DECAF_CAM_SCAN=<s> : <s> secondes après le démarrage, journalise les caméras trouvées.
void scanCameras()
{
    const uint8_t* const base = ptr(0);
    int found = 0;
    for (uint64_t addr = 0x10000000; addr < 0x50000000 && found < 80;)
    {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(base + addr, &info, sizeof(info)))
            break;
        const uint64_t end = uint64_t(static_cast<const uint8_t*>(info.BaseAddress) - base) + info.RegionSize;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            for (uint64_t a = (addr + 3) & ~3ull; a + 84 <= end && found < 80; a += 4)
            {
                float m[12];
                m[0] = loadFloat(base, a);
                if (!(std::fabs(m[0]) <= 1.001f) || m[0] == 0.0f)
                    continue;
                bool ok = true;
                for (int i = 1; i < 12 && ok; i++)
                {
                    m[i] = loadFloat(base, a + i * 4);
                    ok = std::isfinite(m[i]) && (i % 4 == 3 || std::fabs(m[i]) <= 1.001f);
                }
                if (!ok)
                    continue;
                const float* r0 = m;
                const float* r1 = m + 4;
                const float* r2 = m + 8;
                auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
                if (!closeTo(dot(r0, r0), 1, 1e-3f) || !closeTo(dot(r1, r1), 1, 1e-3f) || !closeTo(dot(r2, r2), 1, 1e-3f) ||
                    !closeTo(dot(r0, r1), 0, 1e-3f) || !closeTo(dot(r0, r2), 0, 1e-3f) || !closeTo(dot(r1, r2), 0, 1e-3f))
                    continue;
                // matrice identité (sans rotation) : trop fréquente, sans intérêt
                if (closeTo(m[0], 1, 1e-4f) && closeTo(m[5], 1, 1e-4f) && closeTo(m[10], 1, 1e-4f))
                    continue;
                const float t[3] = { m[3], m[7], m[11] };
                if (std::fabs(t[0]) + std::fabs(t[1]) + std::fabs(t[2]) < 10.0f)
                    continue;
                // position implicite de la caméra : C = -R^T t ; on la cherche autour de la matrice
                const float c[3] = { -(r0[0] * t[0] + r1[0] * t[1] + r2[0] * t[2]),
                                     -(r0[1] * t[0] + r1[1] * t[1] + r2[1] * t[2]),
                                     -(r0[2] * t[0] + r1[2] * t[1] + r2[2] * t[2]) };
                const float eps = 1e-3f * (1.0f + std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[2]));
                int posOffset = 0x7FFF;
                for (int off = -0x60; off <= 0xA0 && posOffset == 0x7FFF; off += 4)
                {
                    if (off > -12 && off < 48)
                        continue;
                    if (closeTo(loadFloat(base, a + off), c[0], eps) && closeTo(loadFloat(base, a + off + 4), c[1], eps) &&
                        closeTo(loadFloat(base, a + off + 8), c[2], eps))
                        posOffset = off;
                }
                found++;
                RT_INFO("CAMÉRA %08X : position (%g %g %g) %s%d, matrice [%g %g %g %g | %g %g %g %g | %g %g %g %g]",
                    uint32_t(a), c[0], c[1], c[2], posOffset == 0x7FFF ? "absente, décalage " : "au décalage ",
                    posOffset == 0x7FFF ? 0 : posOffset, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            }
        addr = end;
    }
    RT_INFO("CAMÉRA : %d candidate(s)", found);
}

void freecamStart()
{
    // DECAF_FREECAM_AT=<s> : active la caméra libre après <s> secondes (test sans clavier)
    if (const char* at = getenv("DECAF_FREECAM_AT"))
        CreateThread(nullptr, 0, [](void* arg) -> DWORD {
            Sleep(DWORD(reinterpret_cast<intptr_t>(arg)) * 1000);
            toggleFreecam();
            return 0;
        }, reinterpret_cast<void*>(intptr_t(atoi(at))), 0, nullptr);
    if (const char* scan = getenv("DECAF_CAM_SCAN"))
    {
        const int seconds = atoi(scan);
        CreateThread(nullptr, 0, [](void* arg) -> DWORD {
            Sleep(DWORD(reinterpret_cast<intptr_t>(arg)) * 1000);
            scanCameras();
            return 0;
        }, reinterpret_cast<void*>(intptr_t(seconds)), 0, nullptr);
    }
}


// Caméra libre (F9). La matrice de vue de la caméra du jeu passe chaque image par
// sub_304D264(objet, vue 3x4, projection, index) (index 0 : caméra du jeu ; index 1 : caméra vue
// du dessus) ; elle est calculée par sub_32198D4(caméra, vue), la mise à jour d'une caméra sead
// « regarder vers » (position en +52, point visé en +64, haut en +76). Quand la caméra libre est
// active, on y écrit avant le calcul la position et le point visé d'une caméra pilotée au clavier
// et à la souris, et le jeu ne reçoit plus de commandes (input.cpp).
namespace
{
std::atomic<bool> s_freecam { false };
std::atomic<bool> s_freecamInit { false }; // reprendre la position de la caméra du jeu
std::atomic<uint32_t> s_mainView { 0 };     // matrice de vue de la caméra du jeu (sub_304D264, index 0)
float s_pos[3] = {};
float s_yaw = 0.0f, s_pitch = 0.0f;
uint64_t s_lastTick = 0;
POINT s_lastCursor {};
bool s_dragging = false;

bool ourWindowFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool scanDown(UINT scanCode)
{
    const UINT vk = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
}

void storeFloatBE(uint8_t* base, uint32_t a, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    memcpy(base + a, &v, 4);
}

void updateFreecam()
{
    const uint64_t now = GetTickCount64();
    const float dt = s_lastTick ? std::fmin(0.1f, float(now - s_lastTick) / 1000.0f) : 0.0f;
    s_lastTick = now;
    // DECAF_FREECAM_DEMO : rotation et montée automatiques (test sans clavier)
    static const bool demo = getenv("DECAF_FREECAM_DEMO") != nullptr;
    if (demo)
    {
        s_yaw += 0.5f * dt;
        s_pos[1] += 60.0f * dt;
    }
    if (!ourWindowFocused())
    {
        s_dragging = false;
        return;
    }
    // Codes de scan (positions QWERTY) : W A S D, E/Q, I J K L
    constexpr UINT W = 0x11, A = 0x1E, S = 0x1F, D = 0x20, E = 0x12, Q = 0x10, I = 0x17, J = 0x24, K = 0x25, L = 0x26;
    const float look = 1.8f * dt;
    s_yaw += (float(scanDown(J)) - float(scanDown(L))) * look;
    s_pitch += (float(scanDown(I)) - float(scanDown(K))) * look;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    {
        POINT p;
        GetCursorPos(&p);
        if (s_dragging)
        {
            s_yaw -= float(p.x - s_lastCursor.x) * 0.004f;
            s_pitch -= float(p.y - s_lastCursor.y) * 0.004f;
        }
        s_lastCursor = p;
        s_dragging = true;
    }
    else
        s_dragging = false;
    s_pitch = std::fmax(-1.55f, std::fmin(1.55f, s_pitch));

    const float f[3] = { std::cos(s_pitch) * std::sin(s_yaw), std::sin(s_pitch), std::cos(s_pitch) * std::cos(s_yaw) };
    const float flat = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    const float r[3] = { -f[2] / flat, 0.0f, f[0] / flat }; // avant x haut (0, 1, 0)
    const bool fast = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const float speed = (fast ? 1600.0f : 400.0f) * dt;
    const float forward = float(scanDown(W)) - float(scanDown(S));
    const float side = float(scanDown(D)) - float(scanDown(A));
    const float up = float(scanDown(E)) - float(scanDown(Q));
    for (int i = 0; i < 3; i++)
        s_pos[i] += (f[i] * forward + r[i] * side) * speed;
    s_pos[1] += up * speed;
}
}

void toggleFreecam() // F9 (gpu_window.cpp)
{
    const bool on = !s_freecam;
    if (on)
        s_freecamInit = true;
    s_freecam = on;
    s_lastTick = 0;
    RT_INFO("caméra libre %s", on ? "activée (WASD/ZQSD, E/Q, IJKL ou clic droit + souris, Maj : vite)" : "désactivée");
}

bool freecamActive()
{
    return s_freecam;
}

} // namespace rt

// Distribution de la matrice de vue au rendu : sub_304D264(objet, vue 3x4, projection, index).
extern "C" void __imp__sub_304D264(PPCContext& __restrict ctx, uint8_t* base);

void sub_304D264(PPCContext& __restrict ctx, uint8_t* base)
{
    using namespace rt;
    static const char* probe = getenv("DECAF_CAM_PROBE");
    static std::atomic<int> logged { 0 };
    if (probe && logged < 60)
    {
        static const auto start = GetTickCount64() + uint64_t(atoi(probe)) * 1000;
        if (GetTickCount64() >= start)
        {
            logged++;
            const uint32_t v = ctx.r4.u32;
            float m[12];
            for (int i = 0; i < 12; i++)
                m[i] = rt::loadFloatBE(base, v + i * 4);
            const float c[3] = { -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]), -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]),
                                 -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11]) };
            RT_INFO("VUE this %08X vue %08X proj %08X index %u lr %08X : caméra (%g %g %g) avant (%g %g %g)", ctx.r3.u32, v,
                ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), c[0], c[1], c[2], -m[8], -m[9], -m[10]);
        }
    }
    if (ctx.r6.u32 == 0)
        s_mainView = ctx.r4.u32; // matrice de vue de la caméra du jeu
    __imp__sub_304D264(ctx, base);
}

// Mise à jour de la matrice d'une caméra sead « regarder vers » (r3 = caméra, r4 = vue 3x4).
extern "C" void __imp__sub_32198D4(PPCContext& __restrict ctx, uint8_t* base);

void sub_32198D4(PPCContext& __restrict ctx, uint8_t* base)
{
    using namespace rt;
    const uint32_t camera = ctx.r3.u32;
    if (s_freecam && ctx.r4.u32 == s_mainView.load())
    {
        if (s_freecamInit.exchange(false))
        {
            float pos[3], at[3];
            for (int i = 0; i < 3; i++)
            {
                pos[i] = loadFloatBE(base, camera + 52 + i * 4);
                at[i] = loadFloatBE(base, camera + 64 + i * 4);
            }
            const float d[3] = { at[0] - pos[0], at[1] - pos[1], at[2] - pos[2] };
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            memcpy(s_pos, pos, sizeof(s_pos));
            if (len > 0.0f)
            {
                s_yaw = std::atan2(d[0], d[2]);
                s_pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, d[1] / len)));
            }
        }
        updateFreecam();
        const float f[3] = { std::cos(s_pitch) * std::sin(s_yaw), std::sin(s_pitch), std::cos(s_pitch) * std::cos(s_yaw) };
        for (int i = 0; i < 3; i++)
        {
            storeFloatBE(base, camera + 52 + i * 4, s_pos[i]);
            storeFloatBE(base, camera + 64 + i * 4, s_pos[i] + f[i] * 100.0f);
            storeFloatBE(base, camera + 76 + i * 4, i == 1 ? 1.0f : 0.0f);
        }
    }
    __imp__sub_32198D4(ctx, base);
}
