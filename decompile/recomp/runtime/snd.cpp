// snd_core (AX) : le moteur audio de la Wii U.
//
// Pas encore de voix ni de sortie son, mais la cadence AX est respectée : un « AX
// Callback Thread » appelle toutes les 3 ms les callbacks de trame de l'application,
// puis les callbacks de mixage final des périphériques (TV, GamePad, Wiimotes) avec des
// tampons silencieux. nw::snd vide sa file de commandes dans ces callbacks : sans eux,
// la file se remplit et tout thread qui envoie une commande audio reste bloqué.
#include "kernel.h"

#include <chrono>
#include <vector>

using namespace rt;

namespace
{

constexpr int kMaxAppFrameCallbacks = 64;
constexpr uint32_t kSamplesPerFrame = 144; // 3 ms à 48 kHz
constexpr uint32_t kMaxChannels = 24;      // jusqu'à 4 périphériques x 6 canaux

struct DeviceLayout
{
    uint16_t devices;
    uint16_t channels;
};
// AXDeviceType : 0 = TV (5.1), 1 = GamePad (4 canaux, 2 manettes), 2 = Wiimote (mono, 4)
constexpr DeviceLayout kDevices[3] = { { 1, 6 }, { 2, 4 }, { 4, 1 } };

std::mutex s_axMutex;
std::vector<uint32_t> s_appFrameCallbacks;
uint32_t s_finalMixCallbacks[3] = {};
bool s_started = false;

// AXDeviceFinalMixData : data (int32_t** +0), channels +4, samples +6, numDevices +8, channelsOut +0xA
uint32_t s_mixData = 0, s_samplePtrs = 0, s_samples = 0;

void axCallbackThread()
{
    PPCContext& ctx = *currentContext();
    s_mixData = sysAlloc(0xC);
    s_samplePtrs = sysAlloc(kMaxChannels * 4);
    s_samples = sysAlloc(kMaxChannels * kSamplesPerFrame * 4);

    auto next = std::chrono::steady_clock::now();
    for (;;)
    {
        next += std::chrono::microseconds(3000);
        const auto now = std::chrono::steady_clock::now();
        if (next < now - std::chrono::milliseconds(100))
            next = now; // trop en retard : on ne rattrape pas les trames perdues
        std::this_thread::sleep_until(next);

        std::vector<uint32_t> appCallbacks;
        uint32_t finalMix[3];
        {
            std::lock_guard lock(s_axMutex);
            appCallbacks = s_appFrameCallbacks;
            memcpy(finalMix, s_finalMixCallbacks, sizeof(finalMix));
        }
        for (uint32_t cb : appCallbacks)
            callGuest(ctx, cb, {});

        for (int type = 0; type < 3; type++)
        {
            if (!finalMix[type])
                continue;
            const auto [devices, channels] = kDevices[type];
            memset(ptr(s_samples), 0, kMaxChannels * kSamplesPerFrame * 4);
            for (uint32_t i = 0; i < uint32_t(devices * channels); i++)
                store32(s_samplePtrs + i * 4, s_samples + i * kSamplesPerFrame * 4);
            store32(s_mixData + 0x0, s_samplePtrs);
            store16(s_mixData + 0x4, channels);
            store16(s_mixData + 0x6, uint16_t(kSamplesPerFrame));
            store16(s_mixData + 0x8, devices);
            store16(s_mixData + 0xA, channels);
            callGuest(ctx, finalMix[type], { s_mixData });
        }
    }
}

} // namespace

HLE_FUNC(snd_core, AXInit)
{
    std::lock_guard lock(s_axMutex);
    if (s_started)
        return;
    s_started = true;
    const auto* t = kernel::current();
    kernel::startServiceThread("AX Callback Thread", t ? t->core : 0, axCallbackThread);
}

HLE_FUNC(snd_core, AXQuit) {}
HLE_FUNC(snd_core, AXUserIsProtected) { RET_U32(0); }

// AXResult AXRegisterAppFrameCallback(AXFrameCallback) : 0 = succès, -17 = invalide, -15 = trop
HLE_FUNC(snd_core, AXRegisterAppFrameCallback)
{
    const uint32_t cb = ARG_U32(3);
    if (!cb)
    {
        RET_S32(-17);
        return;
    }
    std::lock_guard lock(s_axMutex);
    if (s_appFrameCallbacks.size() >= kMaxAppFrameCallbacks)
    {
        RET_S32(-15);
        return;
    }
    s_appFrameCallbacks.push_back(cb);
    RET_S32(0);
}

HLE_FUNC(snd_core, AXDeregisterAppFrameCallback)
{
    std::lock_guard lock(s_axMutex);
    std::erase(s_appFrameCallbacks, ARG_U32(3));
    RET_S32(0);
}

// AXResult AXRegisterDeviceFinalMixCallback(AXDeviceType, AXDeviceFinalMixCallback)
HLE_FUNC(snd_core, AXRegisterDeviceFinalMixCallback)
{
    if (ARG_U32(3) >= 3)
    {
        RET_S32(-1);
        return;
    }
    std::lock_guard lock(s_axMutex);
    s_finalMixCallbacks[ARG_U32(3)] = ARG_U32(4);
    RET_S32(0);
}

// AXResult AXGetDeviceFinalMixCallback(AXDeviceType, AXDeviceFinalMixCallback* out)
HLE_FUNC(snd_core, AXGetDeviceFinalMixCallback)
{
    if (ARG_U32(3) >= 3)
    {
        RET_S32(-1);
        return;
    }
    std::lock_guard lock(s_axMutex);
    store32(ARG_U32(4), s_finalMixCallbacks[ARG_U32(3)]);
    RET_S32(0);
}

// AXResult AXGetDeviceMode(AXDeviceType, AXDeviceMode* out) : 0 = mode par défaut
HLE_FUNC(snd_core, AXGetDeviceMode)
{
    if (ARG_U32(4))
        store32(ARG_U32(4), 0);
    RET_S32(0);
}
