// Export instruction-bounded full-body contracts from the existing, read-only project.
// No analysis, renaming or target execution. Gson is shipped with Ghidra.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import com.google.gson.*;
import java.nio.file.*;
import java.util.*;

public class Discovery extends GhidraScript {
    long base = 0x140000000L;
    JsonArray entries = new JsonArray();
    Map<Long, JsonObject> functions = new LinkedHashMap<>();
    String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format("%02x", b & 255));
        return s.toString();
    }
    JsonObject contract(long key, long start, int length, String name, JsonArray groups) throws Exception {
        Address a = toAddr(base + start);
        byte[] bytes = new byte[length], mask = new byte[length];
        Arrays.fill(mask, (byte)255);
        currentProgram.getMemory().getBytes(a, bytes);
        JsonArray refs = new JsonArray();
        InstructionIterator it = currentProgram.getListing().getInstructions(a, true);
        long end = start + length;
        while (it.hasNext()) {
            Instruction ins = it.next();
            long rva = ins.getAddress().getOffset() - base;
            if (rva >= end) break;
            if (rva < start || rva + ins.getLength() > end) throw new Exception("window splits instruction: " + ins.getAddress());
            int offset = (int)(rva-start), len = ins.getLength();
            for (Reference ref : ins.getReferencesFrom()) {
                long target = ref.getToAddress().getOffset() - base;
                if (target < 0 || target >= 0xd89000) continue;
                boolean flow = ref.getReferenceType().isFlow();
                // Ghidra can attach inferred call targets to indirect instructions.
                // These are evidence, not encoded relative operands.
                if (flow && ins.getFlowType().isComputed()) continue;
                // Internal branch displacements are invariant; preserve them.
                if (flow && target >= start && target < end) continue;
                // Detect the encoded RIP-relative ModRM form, not the printed
                // operand text: Ghidra can render [DAT_xxx] instead of [0x...].
                // Inferred register/absolute references are not rel32 operands.
                long delta = target - (rva + len);
                int location = -1, count = 0;
                for (int j = 1; j + 4 <= len; ++j) {
                    int value = (bytes[offset+j]&255) | ((bytes[offset+j+1]&255)<<8) |
                        ((bytes[offset+j+2]&255)<<16) | ((bytes[offset+j+3]&255)<<24);
                    boolean ripModRm = ((bytes[offset+j-1]&255) & 0xc7) == 0x05;
                    if ((long)value == delta && (flow || ripModRm)) { location = offset+j; ++count; }
                }
                if (!flow && count == 0) continue;
                if (count != 1) throw new Exception("cannot identify unique rel32 operand: " + ins.getAddress() + " " + ins);
                for (int j = 0; j < 4; ++j) mask[location+j] = 0;
                JsonObject edge = new JsonObject();
                edge.addProperty("offset",location); edge.addProperty("end",offset+len);
                edge.addProperty("target",target); edge.addProperty("flow",flow);
                refs.add(edge);
            }
        }
        JsonObject e = new JsonObject();
        e.addProperty("rva",key); e.addProperty("start",start); e.addProperty("name",name);
        e.add("groups",groups); e.addProperty("bytes",hex(bytes)); e.addProperty("mask",hex(mask)); e.add("refs",refs);
        return e;
    }
    JsonObject function(long rva, String name, JsonArray groups) throws Exception {
        if (functions.containsKey(rva)) {
            JsonObject existing = functions.get(rva);
            for (JsonElement g : groups) if (!existing.getAsJsonArray("groups").contains(g)) existing.getAsJsonArray("groups").add(g);
            return existing;
        }
        Function f = getFunctionAt(toAddr(base+rva));
        if (f == null) throw new Exception("no function at " + Long.toHexString(rva));
        long last = f.getBody().getMaxAddress().getOffset()-base;
        int length = (int)(last-rva+1);
        if (length <= 0 || length > 100000) throw new Exception("unbounded function " + f.getName());
        JsonObject e = contract(rva,rva,length,name,groups);
        functions.put(rva,e);
        return e;
    }
    public void run() throws Exception {
        String[] args = getScriptArgs();
        JsonObject input = JsonParser.parseString(Files.readString(Paths.get(args[0]))).getAsJsonObject();
        if (!input.get("reference_sha256").getAsString().equalsIgnoreCase(currentProgram.getExecutableSHA256()))
            throw new Exception("Ghidra executable SHA-256 differs from the reviewed reference; export refused");
        for (JsonElement item : input.getAsJsonArray("functions")) {
            JsonObject f = item.getAsJsonObject();
            function(f.get("rva").getAsLong(),f.get("name").getAsString(),f.getAsJsonArray("groups"));
        }
        JsonArray tables = new JsonArray();
        for (JsonElement item : input.getAsJsonArray("tables")) {
            JsonObject t = item.getAsJsonObject(), out = new JsonObject();
            long rva = t.get("rva").getAsLong();
            out.addProperty("rva",rva); out.addProperty("group",t.get("group").getAsString());
            long col = currentProgram.getMemory().getLong(toAddr(base+rva-8));
            long type = currentProgram.getMemory().getInt(toAddr(col+12)) & 0xffffffffL;
            StringBuilder typeName = new StringBuilder();
            for (int i=0;i<512;++i) { byte b=currentProgram.getMemory().getByte(toAddr(base+type+16+i)); if(b==0)break;typeName.append((char)b); }
            out.addProperty("type",typeName.toString());
            JsonArray slots = new JsonArray(), groups = new JsonArray(); groups.add(t.get("group"));
            for (JsonElement value : t.getAsJsonArray("slots")) {
                int slot = value.getAsInt();
                long target = currentProgram.getMemory().getLong(toAddr(base+rva+slot*8))-base;
                println("Vtable "+Long.toHexString(rva)+" slot "+slot+" -> "+Long.toHexString(target));
                function(target,"vtable_method_"+Long.toHexString(target),groups);
                JsonObject s = new JsonObject(); s.addProperty("slot",slot);s.addProperty("target",target);slots.add(s);
            }
            out.add("slots",slots);tables.add(out);
        }
        for (JsonElement item : input.getAsJsonArray("windows")) {
            JsonObject w=item.getAsJsonObject();
            entries.add(contract(w.get("rva").getAsLong(),w.get("start").getAsLong(),w.get("length").getAsInt(),w.get("name").getAsString(),w.getAsJsonArray("groups")));
        }
        // Retain full native callees as local witnesses, without recursively harvesting the whole game.
        Map<Long,JsonObject> witnesses = new LinkedHashMap<>();
        List<JsonObject> primary = new ArrayList<>(functions.values());
        for (JsonElement e : entries) primary.add(e.getAsJsonObject());
        for (JsonObject e : primary) for (JsonElement edge : e.getAsJsonArray("refs")) {
            JsonObject ref=edge.getAsJsonObject();long target=ref.get("target").getAsLong();
            if (!ref.get("flow").getAsBoolean() || functions.containsKey(target) || witnesses.containsKey(target)) continue;
            Function f=getFunctionAt(toAddr(base+target));
            if(f==null)throw new Exception("flow target without body: "+Long.toHexString(target));
            int length=(int)(f.getBody().getMaxAddress().getOffset()-base-target+1);
            witnesses.put(target,contract(target,target,length,"callee_"+Long.toHexString(target),new JsonArray()));
        }
        for(JsonObject f:functions.values()) entries.add(f);
        JsonArray witnessArray=new JsonArray();for(JsonObject w:witnesses.values())witnessArray.add(w);
        JsonObject output=new JsonObject();output.add("entries",entries);output.add("tables",tables);output.add("witnesses",witnessArray);
        output.add("reference_sha256",input.get("reference_sha256"));
        Files.writeString(Paths.get(args[1]),new GsonBuilder().setPrettyPrinting().create().toJson(output));
        println("Exported "+entries.size()+" contracts, "+tables.size()+" vtables, "+witnessArray.size()+" callee witnesses");
    }
}
