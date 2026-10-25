// snd_core (AX) : le moteur audio de la Wii U.
//
// Les fonctions AX sont celles de decaf (sndcore2 : voix, décodage ADPCM/PCM, mixage des bus,
// callbacks de mixage final), branchées par gpu/bridge/decaf_bridge.cpp. Leur cadence est
// assurée ici : après AXInit, un « AX Callback Thread » exécute toutes les 3 ms une trame AX
// (decafSndRunFrame : callbacks de trame du jeu, mixage, envoi à la sortie son de
// gpu/bridge/sound_out.cpp). nw::snd vide sa file de commandes dans ces callbacks : sans eux, la
// file se remplit et tout thread qui envoie une commande audio reste bloqué.
#include "kernel.h"

#include <chrono>
#include <mutex>

using namespace rt;

namespace rt
{
void decafSndRunFrame(); // gpu/bridge/decaf_bridge.cpp
}

namespace
{

std::mutex s_axMutex;
bool s_started = false;

void axCallbackThread()
{
    auto next = std::chrono::steady_clock::now();
    for (;;)
    {
        next += std::chrono::microseconds(3000);
        const auto now = std::chrono::steady_clock::now();
        if (next < now - std::chrono::milliseconds(100))
            next = now; // trop en retard : on ne rattrape pas les trames perdues
        std::this_thread::sleep_until(next);
        decafSndRunFrame();
    }
}

} // namespace

namespace rt
{

// Appelé après AXInit (decaf) : démarre la cadence AX sur le cœur de l'appelant.
void sndStartFrameThread()
{
    std::lock_guard lock(s_axMutex);
    if (s_started)
        return;
    s_started = true;
    const auto* t = kernel::current();
    kernel::startServiceThread("AX Callback Thread", t ? t->core : 0, axCallbackThread);
}

} // namespace rt
