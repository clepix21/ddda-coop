// Headless: export every function's instructions, for matching two builds of the same exe.
// Output (GHIDRA_OUT env var, default funcs_out.txt), one line per function then per instruction:
//   F <entry hex> <body size>
//   <addr hex> <len> <mnemonic> <op0 objs>|<op1 objs>|...
// where each operand's objects are r:<reg>, s:<hex scalar>, a:<hex address>, separated by commas.
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.nio.charset.StandardCharsets;

public class ExportFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = System.getenv().getOrDefault("GHIDRA_OUT", "funcs_out.txt");
        PrintWriter out = new PrintWriter(new BufferedWriter(new OutputStreamWriter(new FileOutputStream(outPath), StandardCharsets.UTF_8), 1 << 20));
        Listing listing = currentProgram.getListing();
        FunctionIterator it = listing.getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            AddressSetView body = f.getBody();
            out.println("F " + f.getEntryPoint() + " " + body.getNumAddresses());
            InstructionIterator ii = listing.getInstructions(body, true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                StringBuilder sb = new StringBuilder();
                sb.append(ins.getAddress()).append(' ').append(ins.getLength()).append(' ').append(ins.getMnemonicString()).append(' ');
                int n = ins.getNumOperands();
                for (int i = 0; i < n; ++i) {
                    if (i > 0) sb.append('|');
                    Object[] objs = ins.getOpObjects(i);
                    for (int k = 0; k < objs.length; ++k) {
                        if (k > 0) sb.append(',');
                        Object o = objs[k];
                        if (o instanceof Register) sb.append("r:").append(((Register) o).getName());
                        else if (o instanceof Scalar) sb.append("s:").append(Long.toHexString(((Scalar) o).getUnsignedValue()));
                        else if (o instanceof Address) sb.append("a:").append(o.toString());
                        else sb.append("?:").append(o.toString().replace(' ', '_'));
                    }
                }
                out.println(sb.toString());
            }
        }
        out.close();
    }
}
