// Exports basic stats and function list of the current program.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;

public class ExportStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "notes";
        int total = 0, named = 0, thunks = 0, strings = 0;
        try (PrintWriter w = new PrintWriter(new File(out, "functions.tsv"))) {
            w.println("addr\tsize\tname\tthunk");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                total++;
                boolean auto = f.getName().startsWith("FUN_") || f.getName().startsWith("thunk_FUN_");
                if (!auto) named++;
                if (f.isThunk()) thunks++;
                w.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t" + f.isThunk());
            }
        }
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) { if (StringDataInstance.getStringDataInstance(it.next()) != StringDataInstance.NULL_INSTANCE) strings++; }
        try (PrintWriter w = new PrintWriter(new File(out, "stats.txt"))) {
            w.println("language=" + currentProgram.getLanguageID());
            w.println("imageBase=" + currentProgram.getImageBase());
            w.println("functions=" + total + " named=" + named + " thunks=" + thunks);
            w.println("definedStrings=" + strings);
            for (var b : currentProgram.getMemory().getBlocks())
                w.println("block " + b.getName() + " " + b.getStart() + "-" + b.getEnd() + " " + (b.isRead()?"r":"-") + (b.isWrite()?"w":"-") + (b.isExecute()?"x":"-"));
        }
    }
}
