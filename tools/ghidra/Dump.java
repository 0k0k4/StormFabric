import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class Dump extends GhidraScript {
    String vt0(Symbol s) { return s.getAddress().toString(); }
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> cmds = Files.readAllLines(Paths.get(args[0]));
        PrintWriter out = new PrintWriter(new FileWriter(args[1]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Memory mem = currentProgram.getMemory();
        for (String c : cmds) {
            c = c.trim();
            if (c.isEmpty() || c.startsWith("#")) continue;
            String[] t = c.split("\s+");
            out.println("=================== " + c);
            try {
                if (t[0].equals("D")) {
                    Address a = toAddr(t[1]);
                    Function f = getFunctionContaining(a);
                    if (f == null) { f = createFunction(a, null); }
                    if (f == null) { out.println("no function"); continue; }
                    out.println("// function " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                    DecompileResults r = di.decompileFunction(f, 120, monitor);
                    if (r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
                    else out.println("decompile failed: " + r.getErrorMessage());
                } else if (t[0].equals("A")) {
                    Address a = toAddr(t[1]);
                    Function f = getFunctionContaining(a);
                    InstructionIterator it = (f != null) ? currentProgram.getListing().getInstructions(f.getBody(), true)
                        : currentProgram.getListing().getInstructions(a, true);
                    int n = 0;
                    int lim = t.length > 2 ? Integer.parseInt(t[2]) : 100000;
                    while (it.hasNext() && n++ < lim) { Instruction i = it.next(); out.println(i.getAddress() + "  " + i); }
                } else if (t[0].equals("I")) {
                    InstructionIterator it = currentProgram.getListing().getInstructions(toAddr(t[1]), true);
                    int lim = Integer.parseInt(t[2]);
                    while (it.hasNext() && lim-- > 0) { Instruction i = it.next(); out.println(i.getAddress() + "  " + i); }
                } else if (t[0].equals("B")) {
                    Address a = toAddr(t[1]); int len = Integer.parseInt(t[2]);
                    byte[] b = new byte[len]; mem.getBytes(a, b);
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < len; i++) { sb.append(String.format("%02x", b[i] & 0xff)); if ((i % 16) == 15) sb.append("\n"); else sb.append(" "); }
                    out.println(sb);
                } else if (t[0].equals("Q")) {
                    Address a = toAddr(t[1]); int n = Integer.parseInt(t[2]);
                    for (int i = 0; i < n; i++) {
                        long v = mem.getLong(a.add(8L * i));
                        Address tgt = toAddr(v);
                        Function f = null;
                        try { f = getFunctionContaining(tgt); } catch (Exception e) {}
                        out.println(String.format("[%d] %s : %016x  %s", i, a.add(8L*i), v, f != null ? f.getEntryPoint() + " " + f.getName() : ""));
                    }
                } else if (t[0].equals("X")) {
                    Address a = toAddr(t[1]);
                    for (Reference r : getReferencesTo(a)) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        out.println(r.getFromAddress() + " " + r.getReferenceType() + " in " + (f != null ? f.getEntryPoint() + " " + f.getName() : "?"));
                    }
                } else if (t[0].equals("S")) {
                    // string at address
                    Address a = toAddr(t[1]);
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < 256; i++) { byte b = mem.getByte(a.add(i)); if (b == 0) break; sb.append((char)(b & 0xff)); }
                    out.println(sb);
                } else if (t[0].equals("N")) {
                    SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
                    String q = t[1].toLowerCase(); int cnt = 0;
                    while (si.hasNext() && cnt < 200) { Symbol sy = si.next(); if (sy.getName(true).toLowerCase().contains(q)) { out.println(sy.getAddress() + " " + sy.getName(true) + " " + sy.getSymbolType()); cnt++; } }
                } else if (t[0].equals("H")) {
                    Address vt = toAddr(t[1]);
                    long col = mem.getLong(vt.subtract(8));
                    Address colA = toAddr(col);
                    long base = 0x140000000L;
                    long typeRva = mem.getInt(colA.add(12)) & 0xffffffffL;
                    long chdRva = mem.getInt(colA.add(16)) & 0xffffffffL;
                    Address chd = toAddr(base + chdRva);
                    int n = mem.getInt(chd.add(8));
                    long bcaRva = mem.getInt(chd.add(12)) & 0xffffffffL;
                    Address bca = toAddr(base + bcaRva);
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < n; i++) {
                        long bcdRva = mem.getInt(bca.add(4L * i)) & 0xffffffffL;
                        Address bcd = toAddr(base + bcdRva);
                        long tdRva = mem.getInt(bcd) & 0xffffffffL;
                        Address td = toAddr(base + tdRva);
                        StringBuilder nm = new StringBuilder();
                        for (int k = 0; k < 120; k++) { byte b = mem.getByte(td.add(16 + k)); if (b == 0) break; nm.append((char)(b & 0xff)); }
                        int nb = mem.getInt(bcd.add(4));
                        sb.append(nm).append(" (bases=").append(nb).append(")").append((char)10);
                    }
                    out.println(sb);
                } else if (t[0].equals("HA")) {
                    SymbolIterator si2 = currentProgram.getSymbolTable().getAllSymbols(true);
                    long base = 0x140000000L;
                    while (si2.hasNext()) {
                        Symbol sy = si2.next();
                        String nm0 = sy.getName(true);
                        if (!nm0.endsWith("::vftable")) continue;
                        String cls = nm0.substring(0, nm0.length() - 9);
                        if (!cls.startsWith(t[1])) continue;
                        try {
                            Address vt = sy.getAddress();
                            long col = mem.getLong(vt.subtract(8));
                            Address colA = toAddr(col);
                            long chdRva = mem.getInt(colA.add(16)) & 0xffffffffL;
                            Address chd = toAddr(base + chdRva);
                            int n = mem.getInt(chd.add(8));
                            long bcaRva = mem.getInt(chd.add(12)) & 0xffffffffL;
                            Address bca = toAddr(base + bcaRva);
                            StringBuilder sb = new StringBuilder();
                            for (int i = 1; i < n; i++) {
                                long bcdRva = mem.getInt(bca.add(4L * i)) & 0xffffffffL;
                                Address bcd = toAddr(base + bcdRva);
                                long tdRva = mem.getInt(bcd) & 0xffffffffL;
                                Address td = toAddr(base + tdRva);
                                StringBuilder nm = new StringBuilder();
                                for (int k = 0; k < 120; k++) { byte b = mem.getByte(td.add(16 + k)); if (b == 0) break; nm.append((char)(b & 0xff)); }
                                String x = nm.toString(); x = x.replace(".?AU", "").replace(".?AV", "").replace("@@", "");
                                sb.append(i > 1 ? " -> " : "").append(x);
                            }
                            out.println(vt + " " + cls + " : " + sb);
                        } catch (Exception e) { out.println(vt0(sy) + " " + cls + " ERR"); }
                    }
                } else if (t[0].equals("K")) {
                    Address a = toAddr(t[1]);
                    for (Reference r : getReferencesTo(a)) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function f = getFunctionContaining(r.getFromAddress());
                        Instruction ins = getInstructionAt(r.getFromAddress());
                        String found = "";
                        Instruction p = ins;
                        for (int i = 0; i < 8 && p != null; i++) {
                            p = p.getPrevious();
                            if (p == null) break;
                            String s2 = p.toString();
                            if (s2.startsWith("MOV EDX,") || s2.startsWith("LEA EDX,") || s2.startsWith("MOV RDX,") || s2.startsWith("XOR EDX")) { found = s2; break; }
                        }
                        out.println(r.getFromAddress() + " in " + (f != null ? f.getEntryPoint() : null) + " : " + found);
                    }
                } else if (t[0].equals("GE")) {
                    String q = t[1];
                    InstructionIterator ii = currentProgram.getListing().getInstructions(true);
                    while (ii.hasNext()) {
                        Instruction in = ii.next();
                        if (in.toString().endsWith(q)) {
                            Function f = getFunctionContaining(in.getAddress());
                            out.println(in.getAddress() + "  " + in + "   in " + (f != null ? f.getEntryPoint().toString() : "?"));
                        }
                    }
                } else if (t[0].equals("G")) {
                    // grep instructions text (substring), limit 300
                    String q = c.substring(c.indexOf(' ') + 1);
                    InstructionIterator ii = currentProgram.getListing().getInstructions(true);
                    int cnt = 0;
                    while (ii.hasNext() && cnt < 300) {
                        Instruction in = ii.next();
                        if (in.toString().contains(q)) {
                            Function f = getFunctionContaining(in.getAddress());
                            out.println(in.getAddress() + "  " + in + "   in " + (f != null ? f.getEntryPoint().toString() : "?"));
                            cnt++;
                        }
                    }
                } else if (t[0].equals("SIG")) {
                    Address a = toAddr(t[1]); int want = Integer.parseInt(t[2]);
                    Function f = getFunctionAt(a);
                    InstructionIterator ii = currentProgram.getListing().getInstructions(a, true);
                    java.io.ByteArrayOutputStream bs = new java.io.ByteArrayOutputStream();
                    java.io.ByteArrayOutputStream ms = new java.io.ByteArrayOutputStream();
                    int total = 0;
                    while (ii.hasNext() && total < want) {
                        Instruction in = ii.next();
                        byte[] b = in.getBytes();
                        String st = in.toString();
                        boolean mask = in.getFlowType().isCall() || in.getFlowType().isJump() || st.contains("[0x") || st.contains("DAT_");
                        if (st.startsWith("CALL R") || st.startsWith("JMP R") || st.startsWith("CALL qword ptr [R") || st.startsWith("JMP qword ptr [R")) mask = st.contains("[0x");
                        for (int i = 0; i < b.length; i++) { bs.write(b[i]); ms.write(mask ? 0x00 : 0xff); }
                        total += b.length;
                    }
                    byte[] pat = bs.toByteArray(); byte[] msk = ms.toByteArray();
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < pat.length; i++) sb.append(msk[i] == 0 ? "??" : String.format("%02X", pat[i] & 0xff)).append(i + 1 < pat.length ? " " : "");
                    // count matches
                    int cnt = 0; Address cur = currentProgram.getMinAddress();
                    String firstHit = "";
                    while (cnt < 10) {
                        Address h = mem.findBytes(cur, pat, msk, true, monitor);
                        if (h == null) break;
                        cnt++; if (cnt == 1) firstHit = h.toString();
                        cur = h.add(1);
                    }
                    out.println("SIG " + t[1] + " len=" + pat.length + " matches=" + cnt + " first=" + firstHit);
                    out.println(sb);
                } else if (t[0].equals("F")) {
                    // find string occurrences (ASCII) in memory
                    byte[] pat = c.substring(c.indexOf(' ') + 1).getBytes("ISO-8859-1");
                    Address a = mem.getMinAddress();
                    int cnt = 0;
                    while (cnt < 50) {
                        Address f = mem.findBytes(a, pat, null, true, monitor);
                        if (f == null) break;
                        out.println("found at " + f);
                        a = f.add(1); cnt++;
                    }
                }
            } catch (Exception e) {
                out.println("ERR " + e);
            }
            out.flush();
        }
        out.close();
        di.dispose();
    }
}
