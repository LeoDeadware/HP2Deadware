// Pre-analysis for a flat XBE image (tools/xbe_flatten.py): splits the single raw block
// into the XBE sections, labels the entry point and the kernel thunk slots.
//   XbePre.java <default.flat.json> [xboxkrnl_ordinals.txt]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.address.Address;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class XbePre extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] a = getScriptArgs();
    String json = new String(Files.readAllBytes(Paths.get(a[0])));
    Map<Integer, String> names = new HashMap<>();
    if (a.length > 1 && Files.exists(Paths.get(a[1])))
      for (String l : Files.readAllLines(Paths.get(a[1]))) {
        String[] p = l.trim().split("\\s+");
        if (p.length >= 2 && !l.startsWith("#")) { try { names.put(Integer.parseInt(p[0].replace(":", "")), p[1]); } catch (NumberFormatException e) { } }
      }
    Memory mem = currentProgram.getMemory();
    Matcher m = Pattern.compile("\"name\": \"([^\"]+)\",\\s*\"va\": (\\d+)").matcher(json);
    while (m.find()) {
      Address at = toAddr(Long.parseLong(m.group(2)));
      MemoryBlock b = mem.getBlock(at);
      if (b == null) continue;
      if (!b.getStart().equals(at)) { mem.split(b, at); b = mem.getBlock(at); }
      b.setName(m.group(1).replace("$", "_"));
    }
    long entry = Long.parseLong(find(json, "entry"));
    long thunk = Long.parseLong(find(json, "kernel_thunk"));
    createLabel(toAddr(entry), "xbe_entry", true, SourceType.USER_DEFINED);
    addEntryPoint(toAddr(entry));
    Matcher o = Pattern.compile("\"kernel_ordinals\": \\[([^\\]]*)\\]").matcher(json);
    o.find();
    int i = 0;
    for (String s : o.group(1).split(",")) {
      int ord = Integer.parseInt(s.trim());
      String n = names.getOrDefault(ord, "ord_" + ord);
      Address slot = toAddr(thunk + 4L * i++);
      createDWord(slot);
      createLabel(slot, "krnl_" + n, true, SourceType.USER_DEFINED);
    }
    println("XbePre: sections split, " + i + " kernel slots labelled");
  }
  static String find(String j, String k) {
    Matcher m = Pattern.compile("\"" + k + "\": (\\d+)").matcher(j);
    m.find();
    return m.group(1);
  }
}
