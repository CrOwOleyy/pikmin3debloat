import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.*;
// Exporte les fonctions analysées au format TOML [[functions]] de XenonRecomp.
public class export_functions_toml extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = "/home/red/Documents/games/decompile/recomp/config/carrot_functions.toml";
        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out)));
        w.printf("# Fonctions de carrot.rpx (Update v96) exportees depuis Ghidra.%n");
        w.printf("# Zone .text/.syscall uniquement (0x02000000..0x0FFFFFFF).%n%n");
        int n = 0;
        long minA = Long.MAX_VALUE, maxA = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isExternal() || f.isThunk()) continue;
            long addr = f.getEntryPoint().getOffset();
            if (addr < 0x02000000L || addr >= 0x10000000L) continue;
            long size = f.getBody().getNumAddresses();
            w.printf("[[functions]]%naddress = 0x%08X%nsize = 0x%X%n%n", addr, size);
            n++;
            if (addr < minA) minA = addr;
            if (addr > maxA) maxA = addr;
        }
        w.close();
        println("RESULT EXPORTED=" + n + " FROM=0x" + Long.toHexString(minA) + " TO=0x" + Long.toHexString(maxA));
        println("RESULT FILE=" + out);
    }
}
