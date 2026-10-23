// Fenêtre Win32 + pilote Vulkan de libgpu (decaf). Le pilote consomme le ring PM4 sur son
// propre thread et présente l'image TV dans la fenêtre ; le thread principal du processus
// pompe les messages Windows tant que le jeu tourne.
#include <common/log.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include "runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <libcpu/mmu.h>
#include <libgpu/gpu_config.h>
#include <libgpu/gpu_graphicsdriver.h>

#include <thread>
#include <vector>

namespace
{

gpu::GraphicsDriver* s_driver = nullptr;
HWND s_window = nullptr;

// --capture : photographie la zone client de la fenêtre (BMP 32 bits) toutes les 500 ms
// dans capture/, pour vérifier le rendu sans écran (le pilote Vulkan de decaf n'a pas de
// capture intégrée).
void captureLoop()
{
   CreateDirectoryW(L"capture", nullptr);
   for (int index = 0;; index++)
   {
      Sleep(500);
      RECT rc;
      GetClientRect(s_window, &rc);
      const int w = rc.right, h = rc.bottom;
      if (w <= 0 || h <= 0)
         continue;
      HDC windowDc = GetDC(s_window);
      HDC memDc = CreateCompatibleDC(windowDc);
      HBITMAP bitmap = CreateCompatibleBitmap(windowDc, w, h);
      HGDIOBJ old = SelectObject(memDc, bitmap);
      PrintWindow(s_window, memDc, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */);

      BITMAPINFOHEADER bih {};
      bih.biSize = sizeof(bih);
      bih.biWidth = w;
      bih.biHeight = -h; // de haut en bas
      bih.biPlanes = 1;
      bih.biBitCount = 32;
      bih.biCompression = BI_RGB;
      std::vector<uint8_t> pixels(size_t(w) * h * 4);
      GetDIBits(memDc, bitmap, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
      SelectObject(memDc, old);
      DeleteObject(bitmap);
      DeleteDC(memDc);
      ReleaseDC(s_window, windowDc);

      wchar_t name[64];
      swprintf(name, 64, L"capture\\frame_%04d.bmp", index);
      if (FILE* f = _wfopen(name, L"wb"))
      {
         BITMAPFILEHEADER bfh {};
         bfh.bfType = 0x4D42;
         bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
         bfh.bfSize = bfh.bfOffBits + uint32_t(pixels.size());
         fwrite(&bfh, sizeof(bfh), 1, f);
         fwrite(&bih, sizeof(bih), 1, f);
         fwrite(pixels.data(), 1, pixels.size(), f);
         fclose(f);
      }
   }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
   switch (msg)
   {
   case WM_SIZE:
      if (s_driver)
         s_driver->windowSizeChanged(LOWORD(lparam), HIWORD(lparam));
      return 0;
   case WM_CLOSE:
      RT_INFO("fenêtre fermée : arrêt");
      ExitProcess(0);
   }
   return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

namespace rt
{

void tclInit();
void inputSetWindow(void* window);

bool g_captureWindow = false;
uint32_t g_rdocCaptureAt = 0; // --rdoc-capture <s>

// --rdoc-capture <s> : lancé sous RenderDoc (couche Vulkan VK_LAYER_RENDERDOC_Capture), demande la
// capture de l'image suivante après <s> secondes, dans rdoc/. Table RENDERDOC_API_1_6_0 :
// SetCaptureFilePathTemplate = entrée 11, TriggerCapture = entrée 15 (renderdoc_app.h).
static void **rdocApi()
{
   auto module = GetModuleHandleA("renderdoc.dll");
   using GetApi = int (*)(int, void **);
   auto getApi = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
   void **api = nullptr;
   if (!getApi || !getApi(10600, reinterpret_cast<void **>(&api)) || !api) {
      RT_WARN("RenderDoc absent : lancer avec VK_LAYER_PATH=<dossier RenderDoc> VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture");
      return nullptr;
   }
   CreateDirectoryW(L"rdoc", nullptr);
   reinterpret_cast<void (*)(const char *)>(api[11])("rdoc/pikmin3");
   return api;
}

static void rdocCaptureLoop(uint32_t seconds)
{
   // DECAF_RDOC_EVERY=<s> : une capture de plus toutes les <s> secondes (8 au total)
   const char *every = getenv("DECAF_RDOC_EVERY");
   std::this_thread::sleep_for(std::chrono::seconds(seconds));
   for (int i = 0; i < (every ? 8 : 1); i++) {
      if (i)
         std::this_thread::sleep_for(std::chrono::seconds(atoi(every)));
      if (auto api = rdocApi()) {
         reinterpret_cast<void (*)()>(api[15])(); // TriggerCapture
         RT_INFO("RenderDoc : capture demandée");
      }
   }
}

// Capture RenderDoc de tout ce que le GPU exécute pendant <ms> millisecondes à partir de maintenant
// (StartFrameCapture = entrée 19, EndFrameCapture = entrée 21), pour attraper un rendu ponctuel.
void rdocCaptureSpan(uint32_t ms)
{
   std::thread([ms]() {
      auto api = rdocApi();
      if (!api)
         return;
      reinterpret_cast<void (*)(void *, void *)>(api[19])(nullptr, nullptr);
      RT_INFO("RenderDoc : capture ponctuelle commencée");
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      reinterpret_cast<uint32_t (*)(void *, void *)>(api[21])(nullptr, nullptr);
      RT_INFO("RenderDoc : capture ponctuelle terminée");
   }).detach();
}
bool g_splitView = false; // --split : écran TV + GamePad (disposition de decaf)

void gpuStart()
{
   // libgpu lit la mémoire invitée via ces bases ; adresses physiques = virtuelles.
   cpu::internal::BaseVirtualAddress = reinterpret_cast<uintptr_t>(g_base);
   cpu::internal::BasePhysicalAddress = reinterpret_cast<uintptr_t>(g_base);

   // --probe : journal interne de decaf (draws ignorés, erreurs de shader…) sur stderr.
   if (rt::g_probe)
   {
      auto logger = std::make_shared<spdlog::logger>("decaf", std::make_shared<spdlog::sinks::stderr_sink_mt>());
      logger->set_level(spdlog::level::debug);
      gLog = logger;
   }

   WNDCLASSEXW wc {};
   wc.cbSize = sizeof(wc);
   wc.lpfnWndProc = windowProc;
   wc.hInstance = GetModuleHandleW(nullptr);
   wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
   // Fond noir tant que le pilote Vulkan n'a rien présenté (au lieu du blanc par défaut).
   wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
   wc.lpszClassName = L"Pikmin3Recomp";
   RegisterClassExW(&wc);

   RECT rect { 0, 0, 1280, 720 };
   AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
   s_window = CreateWindowExW(0, wc.lpszClassName, L"Pikmin 3 - recompilé", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
   if (!s_window)
      fatal("impossible de créer la fenêtre (%lu)", GetLastError());
   ShowWindow(s_window, SW_SHOW);
   inputSetWindow(s_window);

   // Écran TV seul, sur fond noir (decaf affiche par défaut TV + GamePad sur fond rouge).
   auto settings = *gpu::config();
   settings.display.viewMode = g_splitView ? gpu::DisplaySettings::Split : gpu::DisplaySettings::TV;
   settings.display.backgroundColour = { 0, 0, 0 };
   settings.display.maintainAspectRatio = true;
   // DECAF_DUMP_SHADERS : shaders du jeu (binaire Latte désassemblé + SPIR-V) dans dump/.
   settings.debug.dump_shaders = getenv("DECAF_DUMP_SHADERS") != nullptr;
   gpu::setConfig(settings);

   s_driver = gpu::createGraphicsDriver(gpu::GraphicsDriverType::Vulkan);
   if (!s_driver)
      fatal("impossible de créer le pilote Vulkan");
   gpu::WindowSystemInfo wsi {};
   wsi.type = gpu::WindowSystemType::Windows;
   wsi.renderSurface = s_window;
   s_driver->setWindowSystemInfo(wsi);
   std::thread([] { s_driver->run(); }).detach();

   tclInit();
   if (g_captureWindow)
      std::thread(captureLoop).detach();
   if (g_rdocCaptureAt)
      std::thread(rdocCaptureLoop, g_rdocCaptureAt).detach();
   RT_INFO("rendu : pilote Vulkan (libgpu de decaf) démarré");
}

// Pompe les messages Windows jusqu'à la fin du thread de jeu.
void gpuMessageLoop(void* gameThread)
{
   HANDLE handle = static_cast<HANDLE>(gameThread);
   for (;;)
   {
      const DWORD r = MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_ALLINPUT);
      if (r == WAIT_OBJECT_0)
         return;
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }
}

} // namespace rt
