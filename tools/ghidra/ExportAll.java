// Dumps every function: decompiled C (one file) and an index with block, size, callers.
//   ExportAll.java <out_dir>
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;

public class ExportAll extends GhidraScript {
  @Override
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    new java.io.File(out).mkdirs();
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    int n = 0;
    try (PrintWriter c = new PrintWriter(out + "/decomp.c");
         PrintWriter idx = new PrintWriter(out + "/functions.tsv")) {
      idx.println("addr\tname\tblock\tsize\tcallers");
      for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
        if (monitor.isCancelled()) break;
        StringBuilder callers = new StringBuilder();
        for (Function g : f.getCallingFunctions(monitor))
          callers.append(g.getEntryPoint()).append(' ');
        String block = currentProgram.getMemory().getBlock(f.getEntryPoint()).getName();
        idx.printf("%s\t%s\t%s\t%d\t%s%n", f.getEntryPoint(), f.getName(), block,
                   f.getBody().getNumAddresses(), callers.toString().trim());
        DecompileResults r = d.decompileFunction(f, 60, monitor);
        c.printf("// ==== %s @ %s [%s] ====%n%s%n", f.getName(), f.getEntryPoint(), block,
                 r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// " + r.getErrorMessage());
        n++;
      }
    }
    println("ExportAll: " + n + " functions");
  }
}
