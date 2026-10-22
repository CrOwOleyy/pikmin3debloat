import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
// Compte fonctions/symboles/octets exécutables du programme analysé.
public class ghidra_count extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("RESULT FUNC_COUNT=" + currentProgram.getFunctionManager().getFunctionCount());
        println("RESULT SYMBOL_COUNT=" + currentProgram.getSymbolTable().getNumSymbols());
        long total = 0;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            if (b.isExecute()) total += b.getSize();
        println("RESULT EXEC_BYTES=" + total);
    }
}
