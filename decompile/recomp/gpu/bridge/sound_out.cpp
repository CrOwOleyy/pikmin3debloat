// Sortie son : decaf::SoundDriver au-dessus de waveOut (winmm). snd_core de decaf livre toutes
// les 3 ms 144 échantillons stéréo 16 bits (48 kHz) ; on les regroupe en tampons de 10 ms. Au-delà
// de kMaxQueued tampons en attente (le jeu produit plus vite que la carte son ne consomme), la trame
// est abandonnée pour que la latence ne grandisse pas.
#include "runtime.h"

#include <decaf_sound.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{

class WaveOutDriver : public decaf::SoundDriver
{
public:
   bool start(unsigned outputRate, unsigned numChannels) override
   {
      mChannels = numChannels;
      WAVEFORMATEX format {};
      format.wFormatTag = WAVE_FORMAT_PCM;
      format.nChannels = static_cast<WORD>(numChannels);
      format.nSamplesPerSec = outputRate;
      format.wBitsPerSample = 16;
      format.nBlockAlign = static_cast<WORD>(numChannels * 2);
      format.nAvgBytesPerSec = outputRate * format.nBlockAlign;
      if (waveOutOpen(&mDevice, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
         RT_WARN("son : waveOutOpen a échoué, pas de sortie audio");
         return false;
      }
      mFramesPerBuffer = outputRate / 100; // 10 ms
      for (auto &buffer : mBuffers) {
         buffer.samples.resize(mFramesPerBuffer * numChannels);
         buffer.header = {};
         buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
         buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * 2);
         waveOutPrepareHeader(mDevice, &buffer.header, sizeof(WAVEHDR));
         buffer.header.dwFlags |= WHDR_DONE; // libre
      }
      RT_INFO("son : sortie waveOut %u Hz, %u canaux", outputRate, numChannels);
      return true;
   }

   void output(int16_t *samples, unsigned numSamples) override
   {
      std::lock_guard lock(mMutex);
      if (!mDevice) {
         return;
      }
      auto queued = 0u;
      for (auto &buffer : mBuffers) {
         queued += (buffer.header.dwFlags & WHDR_DONE) ? 0 : 1;
      }
      if (queued >= kMaxQueued && mFill == 0) {
         return; // trop d'avance : on saute cette trame
      }
      for (unsigned i = 0; i < numSamples; ++i) {
         auto &buffer = mBuffers[mCurrent];
         if (mFill == 0 && !(buffer.header.dwFlags & WHDR_DONE)) {
            return; // plus de tampon libre
         }
         std::memcpy(&buffer.samples[mFill * mChannels], &samples[i * mChannels], mChannels * 2);
         if (++mFill == mFramesPerBuffer) {
            buffer.header.dwFlags &= ~WHDR_DONE;
            waveOutWrite(mDevice, &buffer.header, sizeof(WAVEHDR));
            mFill = 0;
            mCurrent = (mCurrent + 1) % mBuffers.size();
         }
      }
   }

   void stop() override
   {
      std::lock_guard lock(mMutex);
      if (mDevice) {
         waveOutReset(mDevice);
         for (auto &buffer : mBuffers) {
            waveOutUnprepareHeader(mDevice, &buffer.header, sizeof(WAVEHDR));
         }
         waveOutClose(mDevice);
         mDevice = nullptr;
      }
   }

private:
   static constexpr unsigned kMaxQueued = 8; // 80 ms

   struct Buffer
   {
      WAVEHDR header;
      std::vector<int16_t> samples;
   };

   std::mutex mMutex;
   HWAVEOUT mDevice = nullptr;
   unsigned mChannels = 2;
   unsigned mFramesPerBuffer = 480;
   std::array<Buffer, 16> mBuffers;
   size_t mCurrent = 0;
   unsigned mFill = 0;
};

} // namespace

namespace rt
{

// --no-sound : pas de sortie audio (le mixage AX tourne quand même)
bool g_noSound = false;

void sndOutputInit()
{
   static WaveOutDriver driver;
   if (!g_noSound && !getenv("DECAF_NO_SOUND")) {
      decaf::setSoundDriver(&driver);
   }
}

} // namespace rt
