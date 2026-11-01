import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

// Offline inventory, never a runtime address fallback. No project writes.
public class CustomDisplayQueueScan extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Expected output path");
        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(
                new FileOutputStream(args[0]), StandardCharsets.UTF_8))) {
            out.println("SHA-256: " + currentProgram.getExecutableSHA256());
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(toAddr("14020c000"), true);
            while (functions.hasNext()) {
                monitor.checkCancelled();
                Function f = functions.next();
                if (f.getEntryPoint().compareTo(toAddr("14020eb00")) >= 0) break;
                out.println("FUNCTION " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                InstructionIterator instructions = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (instructions.hasNext()) {
                    Instruction i = instructions.next();
                    if (i.getFlowType().isCall() || i.getFlowType().isComputed())
                        out.println("  " + i.getAddress() + " " + i);
                }
            }
            out.println("COMPLETE");
            if (out.checkError()) throw new IOException("Could not write queue inventory");
        }
    }
}
