// Exports all non-default symbols and defined strings.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;

public class ExportSyms extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "notes";
        try (PrintWriter w = new PrintWriter(new File(out, "symbols.tsv"), "UTF-8")) {
            w.println("addr\ttype\tsource\tname");
            SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
            while (si.hasNext()) {
                Symbol s = si.next();
                if (s.getSource() == SourceType.DEFAULT) continue;
                w.println(s.getAddress() + "\t" + s.getSymbolType() + "\t" + s.getSource() + "\t" + s.getName(true));
            }
        }
        try (PrintWriter w = new PrintWriter(new File(out, "strings.tsv"), "UTF-8")) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
                if (sdi == StringDataInstance.NULL_INSTANCE) continue;
                String v = sdi.getStringValue();
                if (v == null) continue;
                w.println(d.getAddress() + "\t" + v.replace("\\", "\\\\").replace("\n", "\n").replace("\t", "\t").replace("\r", "\r"));
            }
        }
    }
}
