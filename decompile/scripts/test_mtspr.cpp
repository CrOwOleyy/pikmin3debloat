#include "disasm.h"
#include <cstdio>
#include <cstdint>
int main() {
    uint32_t words[] = { 0x7C62E3A6u /* mtspr 898,r3 (UGQR2) */, 0x7C62E2A6u /* mfspr r3,898 */ };
    for (uint32_t w : words) {
        uint32_t be = __builtin_bswap32(w);
        ppc_insn insn{};
        ppc::Disassemble(&be, 0x2000000, insn);
        printf("%08X -> id=%d name=%s op_str='%s' ops=[%u,%u,%u]\n", w,
               insn.opcode ? insn.opcode->id : -1, insn.opcode ? insn.opcode->name : "(null)",
               insn.op_str, insn.operands[0], insn.operands[1], insn.operands[2]);
    }
}
