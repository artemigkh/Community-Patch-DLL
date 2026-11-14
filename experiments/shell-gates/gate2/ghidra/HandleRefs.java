// Args: outFile inFile(list of hex addrs, one per line)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class HandleRefs extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    BufferedReader br = new BufferedReader(new FileReader(a[1]));
    ReferenceManager rm = currentProgram.getReferenceManager();
    FunctionManager fm = currentProgram.getFunctionManager();
    String line;
    while ((line = br.readLine()) != null) {
      line = line.trim(); if (line.isEmpty()) continue;
      Address ad = toAddr(Long.parseLong(line, 16));
      StringBuilder sb = new StringBuilder(line);
      for (Reference r : rm.getReferencesTo(ad)) {
        Function c = fm.getFunctionContaining(r.getFromAddress());
        sb.append(" ").append(r.getFromAddress()).append(":").append(r.getReferenceType().isWrite() ? "W" : "R").append(":")
          .append(c == null ? "?" : c.getEntryPoint().toString());
      }
      out.println(sb);
    }
    out.close();
  }
}
