// Applies the EXE symbols from LoneGazebo/Community-Patch-DLL PR #13379 (Exe/CvExeAddresses.h)
// to the current program as labels / function names, after checking each byte signature.
// Usage (headless): -postScript ApplyPr13379Symbols.java <path to pr13379_symbols.csv> [dx11|dx9|tablet]
// Column defaults to dx11. Rows of kind EXE_OFFSET are struct offsets and are skipped.
//@category CivV

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CommentType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.FileReader;

public class ApplyPr13379Symbols extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("usage: ApplyPr13379Symbols.java <csv> [dx11|dx9|tablet]");
			return;
		}
		String column = args.length > 1 ? args[1].toLowerCase() : "dx11";
		int col = column.equals("dx9") ? 3 : column.equals("tablet") ? 4 : 2;

		SymbolTable st = currentProgram.getSymbolTable();
		Memory mem = currentProgram.getMemory();
		int applied = 0, skipped = 0, sigFail = 0;

		try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
			String line = r.readLine(); // header
			while ((line = r.readLine()) != null) {
				if (line.isBlank()) continue;
				String[] f = line.split(",", 7);
				String name = f[0].trim(), kind = f[1].trim(), va = f[col].trim();
				String sig = f.length > 5 ? f[5].trim() : "";
				String note = f.length > 6 ? f[6].trim() : "";
				if (kind.equals("EXE_OFFSET")) { skipped++; continue; }
				if (va.equalsIgnoreCase("0xFFFFFFFF") || va.isEmpty()) { skipped++; continue; }

				Address a = toAddr(Long.decode(va));
				if (!sig.isEmpty() && !signatureMatches(mem, a, sig)) {
					printerr(String.format("SIGNATURE MISMATCH %s @ %s - not applied", name, a));
					sigFail++;
					continue;
				}

				if (kind.equals("EXE_FUNCTION")) {
					Function fn = getFunctionAt(a);
					if (fn == null) {
						disassemble(a);
						fn = createFunction(a, name);
					}
					if (fn != null) fn.setName(name, SourceType.USER_DEFINED);
					else createLabel(a, name, true, SourceType.USER_DEFINED);
				} else {
					Symbol s = createLabel(a, name, true, SourceType.USER_DEFINED);
					if (s != null) s.setPrimary();
				}
				String c = "PR #13379 " + kind + (note.isEmpty() ? "" : ": " + note)
					+ (sig.isEmpty() ? " (no signature)" : " (signature verified)");
				currentProgram.getListing().setComment(a, CommentType.PLATE, c);
				println(String.format("applied %-45s %s %s", name, a, kind));
				applied++;
			}
		}
		println(String.format("PR13379 symbols (%s): applied %d, skipped %d (offsets/none), signature failures %d",
			column, applied, skipped, sigFail));
	}

	private boolean signatureMatches(Memory mem, Address a, String sig) throws Exception {
		String[] t = sig.split("\\s+");
		byte[] buf = new byte[t.length];
		if (mem.getBytes(a, buf) != buf.length) return false;
		for (int i = 0; i < t.length; i++) {
			if (t[i].equals("??")) continue;
			if ((byte) Integer.parseInt(t[i], 16) != buf[i]) return false;
		}
		return true;
	}
}
