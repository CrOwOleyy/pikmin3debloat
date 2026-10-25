// Compatibilité decaf : l'infrastructure HLE que le code GX2 de decaf référence (trace des
// appels, configuration, stubs, formatage printf) - implémentée au plus simple.
#include "runtime.h"

#include <cafe/cafe_ppc_interface_trace_host.h>
#include <cafe/libraries/cafe_hle_library.h>
#include <cafe/libraries/cafe_hle_stub.h>
#include <cafe/libraries/coreinit/coreinit_snprintf.h>
#include <decaf_config.h>

#include <functional>

namespace cafe::hle
{
volatile bool FunctionTraceEnabled = false;

void warnStubInvoked(const char* name)
{
   RT_WARN("decaf : fonction non implémentée %s", name);
}
} // namespace cafe::hle

namespace cafe::detail
{
void invoke_trace_host_impl(cpu::Core*, const char*, bool, const RuntimeParamInfo*, size_t)
{
}
} // namespace cafe::detail

namespace cafe::coreinit::internal
{
// decaf reloge ses bibliothèques HLE dans des images RPL générées ; ici les symboles sont
// déjà en mémoire invitée (voir decaf_bridge.cpp).
OSDynLoad_Error relocateHleLibrary(OSDynLoad_ModuleHandle)
{
   return OSDynLoad_Error::OK;
}

int32_t formatStringV(virt_ptr<char> buffer, uint32_t len, virt_ptr<const char> fmt, virt_ptr<va_list> list)
{
   const std::string text = rt::formatGuestVaList(virt_cast<virt_addr>(fmt).getAddress(),
                                                  virt_cast<virt_addr>(list).getAddress());
   if (buffer && len)
   {
      const size_t n = std::min<size_t>(text.size(), len - 1);
      memcpy(buffer.get(), text.data(), n);
      buffer.get()[n] = 0;
   }
   return static_cast<int32_t>(text.size());
}
} // namespace cafe::coreinit::internal

namespace decaf
{
std::shared_ptr<const Settings> config()
{
   static auto settings = std::make_shared<const Settings>();
   return settings;
}

void registerConfigChangeListener(std::function<void(const Settings &)>)
{
}
} // namespace decaf
