// Args: outFile inFile(hex addrs). Prints: addr entry name size firstString
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
public class FuncOf extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    BufferedReader br = new BufferedReader(new FileReader(a[1]));
    FunctionManager fm = currentProgram.getFunctionManager();
    String line;
    while ((line = br.readLine()) != null) {
      line = line.trim(); if (line.isEmpty()) continue;
      Address ad = toAddr(Long.parseLong(line, 16));
      Function f = fm.getFunctionContaining(ad);
      if (f == null) { out.println(line + " ? ? 0 "); continue; }
      StringBuilder sb = new StringBuilder();
      int n = 0;
      for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
        for (Reference r : ins.getReferencesFrom()) {
          Data d = getDataAt(r.getToAddress());
          if (d != null && d.hasStringValue()) { String s = String.valueOf(d.getValue()).replace(' ', '_'); if (s.length() > 60) s = s.substring(0, 60); sb.append(s).append("|"); if (++n >= 4) break; }
        }
        if (n >= 4) break;
      }
      out.println(line + " " + f.getEntryPoint() + " " + f.getName() + " " + f.getBody().getNumAddresses() + " " + sb);
    }
    out.close();
  }
}
