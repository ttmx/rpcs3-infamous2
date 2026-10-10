// Sets r2 (the TOC pointer) for the whole program before analysis, so that TOC-relative loads resolve.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import java.math.BigInteger;
public class SetToc extends GhidraScript {
    public void run() throws Exception {
        Register r2 = currentProgram.getRegister("r2");
        long toc = Long.decode(getScriptArgs()[0]);
        currentProgram.getProgramContext().setRegisterValue(currentProgram.getMinAddress(), currentProgram.getMaxAddress(), new RegisterValue(r2, BigInteger.valueOf(toc)));
        println("r2 = " + Long.toHexString(toc));
    }
}
