// Decompile the function at each given address, creating it first if Ghidra has none there
// (the lifter finds entries Ghidra's analysis misses). Run with -readOnly: nothing is saved.
//   DecompileAt.java <out.c> <hexva>...
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    try (PrintWriter w = new PrintWriter(args[0])) {
      for (int i = 1; i < args.length; i++) {
        Address a = toAddr(Long.parseLong(args[i], 16));
        Function f = getFunctionAt(a);
        if (f == null) {
          disassemble(a);
          new CreateFunctionCmd(a).applyTo(currentProgram);
          f = getFunctionAt(a);
        }
        if (f == null) { w.printf("// %s: could not create%n%n", args[i]); continue; }
        DecompileResults r = d.decompileFunction(f, 60, monitor);
        w.printf("// ==== %s @ %s ====%n%s%n", f.getName(), a,
                 r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage());
      }
    }
  }
}
