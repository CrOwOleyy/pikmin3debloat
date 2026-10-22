#include "disasm.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
struct Case { uint32_t word; const char* expect; };
int main() {
    // encodages construits à la main
    Case cases[] = {
        { 0x1022182Au, "ps_add" },      // ps_add  f1,f2,f3   (XO5=21)
        { 0x108534A0u, "ps_merge10" },  // ps_merge10 f4,f5,f6 (XO10=592)
        { 0xE0232010u, "psq_l" },       // psq_l   f1,16(r3),0,2
        { 0x1022192Eu, "ps_madd" },     // ps_madd (XO5=29) approx
    };
    for (auto& c : cases) {
        uint32_t be = __builtin_bswap32(c.word); // Disassemble lit des octets big-endian
        ppc_insn insn{};
        ppc::Disassemble(&be, 0x2000000, insn);
        printf("%08X -> id=%4d name=%-12s op=%s  [attendu %s] %s\n",
               c.word,
               insn.opcode ? insn.opcode->id : -1,
               insn.opcode ? insn.opcode->name : "(null)",
               insn.op_str,
               c.expect,
               (insn.opcode && strcmp(insn.opcode->name, c.expect)==0) ? "OK" : "??");
    }
    return 0;
}
