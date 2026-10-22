// Counts ARM vs Thumb instructions and functions, and the mnemonic histogram of the current program.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.lang.Register;
import java.io.*;
import java.math.BigInteger;
import java.util.*;

public class ExportIsaStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "notes";
        Register tmode = currentProgram.getRegister("TMode");
        Map<String, Integer> hist = new TreeMap<>();
        long arm = 0, thumb = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            BigInteger t = ins.getValue(tmode, false);
            boolean isThumb = t != null && t.intValue() == 1;
            if (isThumb) thumb++; else arm++;
            String m = (isThumb ? "T:" : "A:") + ins.getMnemonicString().toLowerCase();
            hist.merge(m, 1, Integer::sum);
        }
        long farm = 0, fthumb = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            BigInteger t = currentProgram.getProgramContext().getValue(tmode, f.getEntryPoint(), false);
            if (t != null && t.intValue() == 1) fthumb++; else farm++;
        }
        try (PrintWriter w = new PrintWriter(new File(out, "arm32_isa.tsv"))) {
            w.println("# instructions: arm=" + arm + " thumb=" + thumb + " ; functions: arm=" + farm + " thumb=" + fthumb);
            hist.entrySet().stream().sorted((a, b) -> b.getValue() - a.getValue()).forEach(e -> w.println(e.getValue() + "\t" + e.getKey()));
        }
    }
}
