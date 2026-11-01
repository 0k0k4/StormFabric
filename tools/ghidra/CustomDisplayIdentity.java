import ghidra.app.script.GhidraScript;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

// Read-only identity witness for the existing analyzed program.
public class CustomDisplayIdentity extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter out = new PrintWriter(Files.newBufferedWriter(
                Paths.get(getScriptArgs()[0]), StandardCharsets.UTF_8))) {
            out.println("Program: " + currentProgram.getName());
            out.println("Executable SHA-256: " + currentProgram.getExecutableSHA256());
            out.println("Image base: " + currentProgram.getImageBase());
            out.println("Language: " + currentProgram.getLanguageID());
            out.println("Compiler: " + currentProgram.getCompilerSpec().getCompilerSpecID());
            out.println("Static evidence only; no activation or future-build certification.");
        }
    }
}
