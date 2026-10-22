// h264 : le décodeur vidéo de la Wii U (cinématique d'ouverture openingMovie.mp4).
//
// Pas encore de décodage : on refuse d'ouvrir le décodeur pour que le lecteur de vidéo du jeu
// abandonne la cinématique au lieu d'attendre indéfiniment des images qui n'arrivent pas.
#include "runtime.h"

namespace
{
constexpr int32_t kH264InvalidParameter = 0x1010000; // H264Error (decaf h264_enum.h)
}

// int32_t H264DECMemoryRequirement(profile, level, maxWidth, maxHeight, uint32_t* outMemReq)
HLE_FUNC(h264, H264DECMemoryRequirement)
{
    RT_WARN("h264 : décodeur vidéo absent, cinématique refusée");
    RET_S32(kH264InvalidParameter);
}
