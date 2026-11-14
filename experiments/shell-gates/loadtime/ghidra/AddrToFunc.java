// Maps EXE addresses (preferred-base VAs, one per line, optional "<va> <count> [...]") to the containing
// Ghidra function, plus a label from names.csv (va,name) when the function entry is listed there.
// args: <in file> <out file> [<names.csv>]
// Run on a COPY of the civ5 Ghidra project (see ../LOADTIME.md, "Tooling"), e.g.
//   analyzeHeadless <copy>\civ5 civ5 -process CivilizationV_DX11.exe -noanalysis -readOnly
//     -scriptPath <this dir> -postScript AddrToFunc.java in.txt out.txt names.csv
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class AddrToFunc extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Map<Long, String> names = new HashMap<>();
        if (a.length > 2) {
            for (String l : Files.readAllLines(Paths.get(a[2]))) {
                String[] p = l.split(",", 2);
                if (p.length == 2 && p[0].trim().startsWith("0x")) names.put(Long.parseLong(p[0].trim().substring(2), 16), p[1].trim());
            }
        }
        PrintWriter pw = new PrintWriter(new FileWriter(a[1]));
        FunctionManager fm = currentProgram.getFunctionManager();
        // The analysed project lacks some functions (jump-table code, e.g. 0x403170): create the named ones in
        // memory first (a -readOnly run never saves them).
        TreeMap<Long, String> sorted = new TreeMap<>(names);
        for (Long ep : sorted.keySet()) {
            Address ea = toAddr(ep);
            if (fm.getFunctionAt(ea) == null) {
                new DisassembleCommand(ea, null, true).applyTo(currentProgram, monitor);
                new CreateFunctionCmd(ea).applyTo(currentProgram, monitor);
            }
        }
        for (String l : Files.readAllLines(Paths.get(a[0]))) {
            l = l.trim();
            if (l.isEmpty() || l.startsWith("#")) continue;
            String[] p = l.split("\\s+", 2);
            String hex = p[0].toLowerCase().replace("0x", "");
            long va = Long.parseLong(hex, 16);
            Address ad = toAddr(va);
            Function f = fm.getFunctionContaining(ad);
            String fn = "<no function>";
            String label = "";
            if (f != null) {
                long ep = f.getEntryPoint().getOffset();
                fn = f.getName() + "+0x" + Long.toHexString(va - ep);
                if (names.containsKey(ep)) label = names.get(ep);
            } else {
                Map.Entry<Long, String> e = sorted.floorEntry(va);
                if (e != null && va - e.getKey() < 0x2000) label = "near " + e.getValue() + " +0x" + Long.toHexString(va - e.getKey());
            }
            pw.println(String.format("0x%08x\t%s\t%s\t%s", va, fn, label, p.length > 1 ? p[1] : ""));
        }
        pw.close();
    }
}
