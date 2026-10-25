// Entrées : clavier et manette XInput -> GamePad Wii U (VPAD, canal 0).
//
// Le clavier est lu par position physique (codes de scan) : la disposition « WASD » d'un
// clavier QWERTY correspond à ZQSD sur un AZERTY. Il n'est pris en compte que lorsque la
// fenêtre du jeu a le focus. Correspondance (positions QWERTY) :
//
//   stick gauche  W A S D        stick droit  I J K L       croix   flèches
//   A  Espace     B  Maj gauche  X  E          Y  Q
//   L  1          R  3           ZL Ctrl gauche ZR C
//   +  Entrée     −  Retour arr. HOME  Origine
//   clic stick gauche F          clic stick droit O
//
// Manette XInput, boutons placés comme sur une manette Nintendo : bas = B, droite = A,
// haut = X, gauche = Y ; LB/RB = L/R ; gâchettes = ZL/ZR ; Start = +, Back = −.
#include "runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>

namespace rt
{

// VPADButtons (decaf vpad_enum.h)
namespace vpad
{
constexpr uint32_t Home = 1 << 1, Minus = 1 << 2, Plus = 1 << 3, R = 1 << 4, L = 1 << 5, ZR = 1 << 6,
                   ZL = 1 << 7, Down = 1 << 8, Up = 1 << 9, Right = 1 << 10, Left = 1 << 11, Y = 1 << 12,
                   X = 1 << 13, B = 1 << 14, A = 1 << 15, StickR = 1 << 17, StickL = 1 << 18;
}

struct GamePadInput
{
    uint32_t buttons = 0;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};

namespace
{

std::atomic<HWND> s_window { nullptr };

struct KeyBinding
{
    UINT key;    // code de scan (position physique) ou code virtuel si virtualKey
    bool virtualKey;
    uint32_t button;
};

// Codes de scan des positions QWERTY.
constexpr UINT SC_W = 0x11, SC_A = 0x1E, SC_S = 0x1F, SC_D = 0x20, SC_I = 0x17, SC_J = 0x24, SC_K = 0x25,
               SC_L = 0x26, SC_E = 0x12, SC_Q = 0x10, SC_C = 0x2E, SC_F = 0x21, SC_O = 0x18, SC_1 = 0x02,
               SC_3 = 0x04;

const KeyBinding kButtons[] = {
    { VK_SPACE, true, vpad::A },
    { VK_LSHIFT, true, vpad::B },
    { SC_E, false, vpad::X },
    { SC_Q, false, vpad::Y },
    { SC_1, false, vpad::L },
    { SC_3, false, vpad::R },
    { VK_LCONTROL, true, vpad::ZL },
    { SC_C, false, vpad::ZR },
    { VK_RETURN, true, vpad::Plus },
    { VK_BACK, true, vpad::Minus },
    { VK_HOME, true, vpad::Home },
    { VK_UP, true, vpad::Up },
    { VK_DOWN, true, vpad::Down },
    { VK_LEFT, true, vpad::Left },
    { VK_RIGHT, true, vpad::Right },
    { SC_F, false, vpad::StickL },
    { SC_O, false, vpad::StickR },
};

bool keyDown(UINT key, bool virtualKey)
{
    const UINT vk = virtualKey ? key : MapVirtualKeyW(key, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
}

// Axe d'un stick clavier : -1, 0 ou +1, diagonales ramenées sur le cercle unité.
void keyStick(UINT up, UINT left, UINT down, UINT right, float& x, float& y)
{
    float kx = float(keyDown(right, false)) - float(keyDown(left, false));
    float ky = float(keyDown(up, false)) - float(keyDown(down, false));
    if (kx && ky)
    {
        kx *= 0.70710678f;
        ky *= 0.70710678f;
    }
    if (kx || ky)
    {
        x = kx;
        y = ky;
    }
}

float thumb(SHORT value, SHORT deadzone)
{
    if (std::abs(int(value)) <= deadzone)
        return 0.0f;
    const float v = value < 0 ? float(value + deadzone) / float(32768 - deadzone)
                              : float(value - deadzone) / float(32767 - deadzone);
    return std::fmax(-1.0f, std::fmin(1.0f, v));
}

void sampleXInput(GamePadInput& in)
{
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        XINPUT_STATE st {};
        if (XInputGetState(i, &st) != ERROR_SUCCESS)
            continue;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        const WORD w = g.wButtons;
        if (w & XINPUT_GAMEPAD_B) in.buttons |= vpad::A;
        if (w & XINPUT_GAMEPAD_A) in.buttons |= vpad::B;
        if (w & XINPUT_GAMEPAD_Y) in.buttons |= vpad::X;
        if (w & XINPUT_GAMEPAD_X) in.buttons |= vpad::Y;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) in.buttons |= vpad::L;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) in.buttons |= vpad::R;
        if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= vpad::ZL;
        if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= vpad::ZR;
        if (w & XINPUT_GAMEPAD_START) in.buttons |= vpad::Plus;
        if (w & XINPUT_GAMEPAD_BACK) in.buttons |= vpad::Minus;
        if (w & XINPUT_GAMEPAD_DPAD_UP) in.buttons |= vpad::Up;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN) in.buttons |= vpad::Down;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT) in.buttons |= vpad::Left;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT) in.buttons |= vpad::Right;
        if (w & XINPUT_GAMEPAD_LEFT_THUMB) in.buttons |= vpad::StickL;
        if (w & XINPUT_GAMEPAD_RIGHT_THUMB) in.buttons |= vpad::StickR;
        in.leftX = thumb(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        in.leftY = thumb(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        in.rightX = thumb(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        in.rightY = thumb(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        return; // première manette connectée
    }
}

} // namespace

void inputSetWindow(void* window)
{
    s_window = static_cast<HWND>(window);
}

// --auto-a <ms> : appuie sur A pendant 100 ms toutes les <ms> millisecondes (tests sans joueur).
uint32_t g_autoA = 0;

GamePadInput inputSampleGamePad()
{
    GamePadInput in;
    sampleXInput(in);
    if (g_autoA && GetTickCount64() % g_autoA < 100)
        in.buttons |= vpad::A;
    // DECAF_AUTO_STICK=<s> : après <s> secondes, stick gauche par phases de 1,5 s (bas, repos,
    // gauche, repos…) pour filmer la marche sans joueur.
    static const char* autoStick = getenv("DECAF_AUTO_STICK");
    static const uint64_t start = GetTickCount64();
    if (autoStick && GetTickCount64() - start > strtoull(autoStick, nullptr, 10) * 1000)
    {
        const uint64_t phase = (GetTickCount64() - start) / 1500 % 4;
        if (phase == 0)
            in.leftY = -1.0f;
        else if (phase == 2)
            in.leftX = -1.0f;
    }
    const HWND window = s_window.load();
    if (window && GetForegroundWindow() == window)
    {
        for (const auto& b : kButtons)
            if (keyDown(b.key, b.virtualKey))
                in.buttons |= b.button;
        keyStick(SC_W, SC_A, SC_S, SC_D, in.leftX, in.leftY);
        keyStick(SC_I, SC_J, SC_K, SC_L, in.rightX, in.rightY);
    }
    return in;
}

} // namespace rt
