// Print the functions that reference each given string (exact match), one line per reference:
//   <string> <ref va> <function entry>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class XrefFuncs extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    try (PrintWriter w = new PrintWriter(args[0])) {
      for (int i = 1; i < args.length; i++) {
        if (args[i].startsWith("@")) {  // @<hexva>: references to that address
          for (Reference r : getReferencesTo(toAddr(Long.parseLong(args[i].substring(1), 16)))) {
            Function f = getFunctionContaining(r.getFromAddress());
            w.printf("%s %s %s%n", args[i], r.getFromAddress(), f == null ? "-" : f.getEntryPoint());
          }
          continue;
        }
        byte[] needle = (args[i] + "\0").getBytes("ISO-8859-1");
        Address a = currentProgram.getMinAddress();
        while ((a = find(a, needle)) != null) {
          for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            w.printf("%s %s %s%n", args[i], r.getFromAddress(), f == null ? "-" : f.getEntryPoint());
          }
          a = a.add(1);
        }
      }
    }
  }
}
