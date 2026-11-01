import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

// Read-only offline evidence. Unlike Dump.java's G command, does not silently
// stop after 300 matches. Arguments: output-file instruction-substring [...].
public class FindInstructionUses extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("Expected output-file and instruction substrings");
        }
        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(
                new FileOutputStream(args[0]), StandardCharsets.UTF_8))) {
            out.println("Program: " + currentProgram.getName());
            out.println("Image base: " + currentProgram.getImageBase());
            out.println("SHA-256: " + currentProgram.getExecutableSHA256());
            for (int n = 1; n < args.length; n++) out.println("Needle: " + args[n]);
            long scanned = 0;
            long matched = 0;
            InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext()) {
                monitor.checkCancelled();
                Instruction instruction = instructions.next();
                scanned++;
                String text = instruction.toString();
                boolean match = false;
                for (int n = 1; n < args.length; n++) {
                    if (text.contains(args[n])) { match = true; break; }
                }
                if (!match) continue;
                matched++;
                Function function = getFunctionContaining(instruction.getAddress());
                out.println(instruction.getAddress() + "  " + text + "   in " +
                        (function == null ? "?" : function.getEntryPoint() + " " + function.getName()));
            }
            out.println("COMPLETE: scanned=" + scanned + " matched=" + matched);
            if (out.checkError()) throw new IOException("Could not write instruction evidence");
        }
    }
}
