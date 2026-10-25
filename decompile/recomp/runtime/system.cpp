// proc_ui, vpad/padscore (entrées), nn_* (comptes, réseau…) :
// comportements minimaux d'une console hors ligne, avec le GamePad branché et
// aucune Wiimote.
#include "kernel.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace rt;

// ================================================================= proc_ui
// ProcUIStatus : 0 = au premier plan, 1 = en arrière-plan, 2 = libérer le premier plan, 3 = quitter
HLE_FUNC(proc_ui, ProcUIInit) {}
HLE_FUNC(proc_ui, ProcUIProcessMessages) { RET_U32(0); }
HLE_FUNC(proc_ui, ProcUIRegisterCallback) {}
HLE_FUNC(proc_ui, ProcUIDrawDoneRelease) {}
HLE_FUNC(proc_ui, ProcUIShutdown) {}

// ================================================================= vpad (GamePad)
namespace rt
{
struct GamePadInput
{
    uint32_t buttons = 0;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};
GamePadInput inputSampleGamePad(); // input.cpp : clavier + manette XInput
bool windowTouch(float& x, float& y); // gpu_window.cpp : souris -> écran tactile
}

// --record-input <fichier> : note chaque changement de commandes (boutons, sticks, tactile) avec le
// numéro de lecture de la manette (le jeu la lit une fois par image) ; --replay-input <fichier> :
// rejoue ces commandes au même numéro de lecture, puis rend la main aux vraies commandes. Le jeu
// n'étant pas parfaitement déterministe ici (threads en parallèle), le rejeu peut dériver.
namespace rt
{
std::string g_recordInput, g_replayInput;
}

namespace
{
struct InputSample
{
    uint64_t index;
    uint32_t buttons;
    float lx, ly, rx, ry;
    int touched;
    float tx, ty;
};

void inputRecordReplay(GamePadInput& in, bool& touched, float& tx, float& ty)
{
    static std::mutex lock;
    std::lock_guard guard(lock);
    static uint64_t index = 0;
    const uint64_t n = index++;

    static std::vector<InputSample> replay;
    static size_t next = 0;
    static bool loaded = false, replaying = false;
    if (!loaded)
    {
        loaded = true;
        if (!rt::g_replayInput.empty())
            if (FILE* f = fopen(rt::g_replayInput.c_str(), "r"))
            {
                InputSample e;
                while (fscanf(f, "%llu %x %f %f %f %f %d %f %f", &e.index, &e.buttons, &e.lx, &e.ly, &e.rx, &e.ry,
                           &e.touched, &e.tx, &e.ty) == 9)
                    replay.push_back(e);
                fclose(f);
                replaying = !replay.empty();
                RT_INFO("rejeu des commandes : %zu changements (%s)", replay.size(), rt::g_replayInput.c_str());
            }
    }
    if (replaying)
    {
        static InputSample current {};
        while (next < replay.size() && replay[next].index <= n)
            current = replay[next++];
        if (next >= replay.size() && n > replay.back().index + 30)
        {
            replaying = false;
            RT_INFO("rejeu des commandes terminé (lecture %llu) : commandes réelles", (unsigned long long)n);
        }
        else
        {
            in.buttons = current.buttons;
            in.leftX = current.lx; in.leftY = current.ly; in.rightX = current.rx; in.rightY = current.ry;
            touched = current.touched != 0;
            tx = current.tx; ty = current.ty;
            return;
        }
    }

    if (!rt::g_recordInput.empty())
    {
        static FILE* f = fopen(rt::g_recordInput.c_str(), "w");
        static InputSample last { ~0ull };
        const InputSample cur { n, in.buttons, in.leftX, in.leftY, in.rightX, in.rightY, touched ? 1 : 0, tx, ty };
        if (f && (last.index == ~0ull || cur.buttons != last.buttons || cur.lx != last.lx || cur.ly != last.ly ||
                  cur.rx != last.rx || cur.ry != last.ry || cur.touched != last.touched ||
                  (cur.touched && (cur.tx != last.tx || cur.ty != last.ty))))
        {
            fprintf(f, "%llu %x %g %g %g %g %d %g %g\n", (unsigned long long)n, cur.buttons, cur.lx, cur.ly, cur.rx,
                cur.ry, cur.touched, cur.tx, cur.ty);
            fflush(f);
            last = cur;
        }
    }
}
}

// int32_t VPADRead(chan, VPADStatus* buffers, uint32_t count, VPADReadError* error)
// VPADStatus (decaf vpad_controller.h) : hold +0, trigger +4, release +8, leftStick +0xC,
// rightStick +0x14, tpNormal +0x52, tpFiltered1 +0x5A, tpFiltered2 +0x62, battery +0xA1
// (taille 0xAC). VPADReadError : 0 = succès, -1 = aucun échantillon, -2 = manette invalide.
HLE_FUNC(vpad, VPADRead)
{
    static std::mutex lock;
    static uint32_t lastButtons = 0;
    const uint32_t buf = ARG_U32(4), count = ARG_U32(5), err = ARG_U32(6);
    if (count == 0 || ARG_U32(3) != 0)
    {
        if (err)
            store32(err, count == 0 ? uint32_t(-1) : uint32_t(-2));
        RET_S32(0);
        return;
    }
    GamePadInput in = inputSampleGamePad();
    float tx = 0, ty = 0;
    bool touched = windowTouch(tx, ty);
    inputRecordReplay(in, touched, tx, ty);
    uint32_t trigger, release;
    {
        std::lock_guard guard(lock);
        trigger = in.buttons & ~lastButtons;
        release = lastButtons & ~in.buttons;
        lastButtons = in.buttons;
    }
    // Journal : à chaque changement de bouton, et quand un stick bouge d'au moins 0,25 (ou
    // revient à zéro) depuis la dernière valeur journalisée - sans noyer le journal sous
    // les variations continues d'un stick analogique.
    static float logged[4] = {};
    const float axes[4] = { in.leftX, in.leftY, in.rightX, in.rightY };
    bool stickChanged = false;
    for (int i = 0; i < 4; i++)
        if (std::fabs(axes[i] - logged[i]) >= 0.25f || (axes[i] == 0.0f && logged[i] != 0.0f))
            stickChanged = true;
    if (trigger || release || stickChanged)
    {
        memcpy(logged, axes, sizeof(logged));
        RT_INFO("GamePad : boutons 0x%05X (+0x%05X -0x%05X) stick G (%.2f, %.2f) D (%.2f, %.2f)", in.buttons,
            trigger, release, in.leftX, in.leftY, in.rightX, in.rightY);
    }
    auto storeFloat = [](uint32_t addr, float v) {
        uint32_t u;
        memcpy(&u, &v, 4);
        store32(addr, u);
    };
    memset(ptr(buf), 0, 0xAC);
    store32(buf + 0x00, in.buttons);
    store32(buf + 0x04, trigger);
    store32(buf + 0x08, release);
    storeFloat(buf + 0x0C, in.leftX);
    storeFloat(buf + 0x10, in.leftY);
    storeFloat(buf + 0x14, in.rightX);
    storeFloat(buf + 0x18, in.rightY);
    // Écran tactile (souris, gpu_window.cpp) : coordonnées brutes 0..4096, y vers le haut (decaf)
    for (uint32_t tp : { 0x52u, 0x5Au, 0x62u })
    {
        if (touched)
        {
            store16(buf + tp + 0, uint16_t(std::fmin(tx * 4096.0f, 4095.0f)));
            store16(buf + tp + 2, uint16_t(std::fmin((1.0f - ty) * 4096.0f, 4095.0f)));
            store16(buf + tp + 4, 1);
        }
        else
            store16(buf + tp + 6, 3); // pas de contact (InvalidX | InvalidY)
    }
    store8(buf + 0xA1, 6);        // batterie pleine
    if (err)
        store32(err, 0);
    RET_S32(1);
}

// Calibrage par défaut de decaf : brut 0..4096 -> 1280x720, y retourné.
HLE_FUNC(vpad, VPADGetTPCalibratedPoint)
{
    const uint32_t out = ARG_U32(4), in = ARG_U32(5);
    const uint16_t x = load16(in), y = load16(in + 2), touched = load16(in + 4), validity = load16(in + 6);
    store16(out + 0, uint16_t(x * 1280.0f / 4096.0f));
    store16(out + 2, uint16_t((4096 - y) * 720.0f / 4096.0f));
    store16(out + 4, touched);
    store16(out + 6, validity);
}

// ================================================================= padscore (Wiimote)
// int32_t KPADReadEx(chan, KPADStatus*, uint32_t size, int32_t* error)
HLE_FUNC(padscore, KPADReadEx)
{
    if (ARG_U32(6))
        store32(ARG_U32(6), uint32_t(-2)); // KPAD_ERROR_NO_CONTROLLER
    RET_S32(0);
}

// int32_t WPADProbe(chan, uint32_t* type) : -1 = pas de manette
HLE_FUNC(padscore, WPADProbe)
{
    if (ARG_U32(4))
        store32(ARG_U32(4), 0xFD);
    RET_S32(-1);
}
HLE_FUNC(padscore, KPADGetMplsWorkSize) { RET_U32(0x5FE0); }

// ================================================================= nn_act (comptes)
// nn::Result : 0 = succès
HLE_FUNC(nn_act, Initialize__Q2_2nn3actFv) { RET_U32(0); }
HLE_FUNC(nn_act, Finalize__Q2_2nn3actFv) { RET_U32(0); }
HLE_FUNC(nn_act, GetNumOfAccounts__Q2_2nn3actFv) { RET_U32(1); }
HLE_FUNC(nn_act, GetSlotNo__Q2_2nn3actFv) { RET_U32(1); }
HLE_FUNC(nn_act, GetPersistentId__Q2_2nn3actFv) { RET_U32(0x80000001); }
HLE_FUNC(nn_act, GetPersistentIdEx__Q2_2nn3actFUc) { RET_U32(0x80000001); }
HLE_FUNC(nn_act, IsNetworkAccount__Q2_2nn3actFv) { RET_U32(0); }
HLE_FUNC(nn_act, HasNfsAccount__Q2_2nn3actFv) { RET_U32(0); }
HLE_FUNC(nn_act, GetPrincipalId__Q2_2nn3actFv) { RET_U32(0); }
HLE_FUNC(nn_act, GetAccountId__Q2_2nn3actFPc)
{
    writeString(ARG_U32(3), "pikmin");
    RET_U32(0);
}
