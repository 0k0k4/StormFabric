import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;

// Offline, selected instruction-span review. Never runs or attaches to the target.
public class ReviewBindings extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[1]))) {
            out.println("OFFLINE DISASSEMBLY: " + currentProgram.getExecutableSHA256());
            for (String line : Files.readAllLines(Paths.get(args[0]))) {
                if (line.trim().isEmpty() || line.startsWith("#")) continue;
                String[] parts = line.trim().split("\\s+");
                Address start = currentProgram.getImageBase().add(Long.decode(parts[0]));
                long size = Long.decode(parts[1]);
                Address end = start.add(size-1);
                out.println("SPAN " + line);
                // Linear bounded decoding retains exact instruction offsets, even if
                // Ghidra has not previously analysed this function.
                Address at = start;
                while (at.compareTo(end) <= 0) {
                    Instruction ins = getInstructionAt(at);
                    if (ins == null) {
                        ghidra.app.cmd.disassemble.DisassembleCommand cmd = new ghidra.app.cmd.disassemble.DisassembleCommand(at, new AddressSet(at,end), false);
                        cmd.applyTo(currentProgram, monitor);
                        ins = getInstructionAt(at);
                    }
                    if (ins == null) { out.println(at + " UNDECODED"); at=at.add(1); continue; }
                    out.println(String.format("+0x%x %s %s", at.subtract(start), at, ins));
                    at=at.add(ins.getLength());
                }
            }
        }
    }
}
