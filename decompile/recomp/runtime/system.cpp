// proc_ui, vpad/padscore (entrées), nn_* (comptes, réseau…) :
// comportements minimaux d'une console hors ligne, avec le GamePad branché et
// aucune Wiimote.
#include "kernel.h"

#include <cmath>

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
    const GamePadInput in = inputSampleGamePad();
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
    for (uint32_t tp : { 0x52u, 0x5Au, 0x62u })
        store16(buf + tp + 6, 3); // écran tactile : pas de contact (InvalidX | InvalidY)
    store8(buf + 0xA1, 6);        // batterie pleine
    if (err)
        store32(err, 0);
    RET_S32(1);
}

HLE_FUNC(vpad, VPADGetTPCalibratedPoint)
{
    memcpy(ptr(ARG_U32(4)), ptr(ARG_U32(5)), 0xA);
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
