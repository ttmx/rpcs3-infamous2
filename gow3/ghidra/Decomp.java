// Decompiles the functions containing the given addresses (hex) to <out dir>/<address>.c; first argument is the out dir.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;
public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address ad = toAddr(Long.parseLong(a[i], 16));
            Function f = getFunctionContaining(ad);
            if (f == null) { disassemble(ad); f = createFunction(ad, null); }
            if (f == null) { println("no function at " + a[i]); continue; }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            try (PrintWriter w = new PrintWriter(new File(a[0], a[i] + ".c"))) {
                w.println("// function " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "FAILED: " + r.getErrorMessage());
            }
            println("wrote " + a[i] + " (" + f.getEntryPoint() + ")");
        }
    }
}
