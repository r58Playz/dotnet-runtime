import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.FieldInsnNode;
import org.objectweb.asm.tree.InsnList;
import org.objectweb.asm.tree.InsnNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;
import org.objectweb.asm.tree.TypeInsnNode;

/*
 * Bytecode patches for mctick's TeaVM arm, applied at build time to classes in client.jar and the libraries (so no
 * Mojang code lives in this repository) and written into the TeaVM class directory, which precedes them on TeaVM's
 * class path. The JVM arms run the jars unmodified.
 *
 *   java -cp <asm.jar>:<asm-tree.jar> PatchClasses.java <out classes dir> <jar>...
 *
 * Every patch removes a path TeaVM cannot link (it analyses every branch, so a fallback the JVM never takes still has
 * to link) and keeps what the JVM arms do in this bench:
 *   net/minecraft/util/profiling/jfr/JvmProfiler  INSTANCE = new NoOpProfiler() instead of probing the module layer for
 *       jdk.jfr. On the JVM arms JFR is present and never started (JfrProfiler's hooks return while not recording);
 *       NoOpProfiler's hooks do nothing: the same work less a field test.
 *   com/google/common/hash/ChecksumHashFunction$ChecksumMethodHandles  no MethodHandle for Checksum.update(ByteBuffer)
 *       (a signature-polymorphic invokeExact TeaVM cannot link): updateByteBuffer answers false and Guava feeds the
 *       checksum from an array, its pre-Java-9 path.
 *   org/joml/MemUtil  createInstance() returns JOML's NIO implementation without trying its Unsafe one (which finds
 *       Unsafe by reflecting over its fields; no reflection metadata on this target). The JVM arms use the Unsafe one.
 *   org/joml/Runtime  hasFloatToRawIntBits / hasDoubleToRawLongBits / hasLongRotateLeft / hasMathFma answer true, as on
 *       a Java 25 JVM. JOML is compiled for Java 1.4, so its probes find java.lang.Float etc. through Class.forName
 *       (javac's old class$ literals), which this target cannot answer.
 *   io/netty/util/ResourceLeakDetector  addExclusions does nothing: it checks by reflection that the named methods
 *       exist (no reflection metadata on this target) before adding them to the leak report's stack filter, and leak
 *       reports are off (netty's default sampling never reports in a run this short; the JVM arms behave the same).
 *   com/mojang/jtracy/TracyBindings  the native methods become methods returning 0/null: Tracy is never loaded in this
 *       bench on any arm (TracyClient tests that first), but the calls must link.
 * and, in every class of every jar, the single-threaded lowering: calls to Thread.yield / sleep / join and Object.wait
 * become calls to wjteavm.Background (bench/teavm-classlib), which runs queued background work and returns. TeaVM's own
 * versions suspend (@Async), and one reachable suspending method makes TeaVM coroutine-transform all its transitive
 * callers: before this pass, 94,471 methods of mctick (2026-11-19, TeaVM's wj.asyncReport, patch 0004), through
 * BlockableEventLoop.waitForTasks's Thread.yield alone most of the chunk system.
 */
public final class PatchClasses {
    private PatchClasses() {
    }

    interface Patch {
        void apply(ClassNode cls);
    }

    public static void main(String[] args) throws Exception {
        Path out = Paths.get(args[0]);
        List<ZipFile> jars = new ArrayList<>();
        for (int i = 1; i < args.length; ++i) {
            jars.add(new ZipFile(args[i]));
        }
        java.util.Map<String, Patch> specific = new java.util.HashMap<>();
        specific.put("net/minecraft/util/profiling/jfr/JvmProfiler", PatchClasses::jvmProfiler);
        specific.put("com/google/common/hash/ChecksumHashFunction$ChecksumMethodHandles", cls -> {
            stub(cls, "<clinit>", "()V");
            stub(cls, "updateByteBuffer", "(Ljava/util/zip/Checksum;Ljava/nio/ByteBuffer;)Z");
            stub(cls, "updateByteBuffer", "()Ljava/lang/invoke/MethodHandle;");
        });
        specific.put("org/joml/MemUtil", cls -> {
            for (MethodNode m : cls.methods) {
                if (m.name.equals("createInstance") && m.desc.equals("()Lorg/joml/MemUtil;")) {
                    InsnList code = new InsnList();
                    code.add(new TypeInsnNode(Opcodes.NEW, "org/joml/MemUtil$MemUtilNIO"));
                    code.add(new InsnNode(Opcodes.DUP));
                    code.add(new MethodInsnNode(Opcodes.INVOKESPECIAL, "org/joml/MemUtil$MemUtilNIO", "<init>", "()V",
                            false));
                    code.add(new InsnNode(Opcodes.ARETURN));
                    m.instructions = code;
                    m.tryCatchBlocks.clear();
                    m.localVariables = null;
                    return;
                }
            }
            throw new IllegalStateException("org/joml/MemUtil.createInstance not found");
        });
        specific.put("org/joml/Runtime", cls -> {
            for (String m : new String[] { "hasFloatToRawIntBits", "hasDoubleToRawLongBits", "hasLongRotateLeft",
                    "hasMathFma" }) {
                returnTrue(cls, m, "()Z");
            }
        });
        specific.put("io/netty/util/ResourceLeakDetector", cls -> {
            stub(cls, "addExclusions", "(Ljava/lang/Class;[Ljava/lang/String;)V");
        });
        specific.put("com/mojang/jtracy/TracyBindings", cls -> {
            int n = 0;
            for (MethodNode m : cls.methods) {
                if ((m.access & Opcodes.ACC_NATIVE) != 0) {
                    m.access &= ~Opcodes.ACC_NATIVE;
                    m.instructions = new InsnList();
                    m.tryCatchBlocks = new ArrayList<>();
                    stub(cls, m.name, m.desc);
                    ++n;
                }
            }
            if (n == 0) {
                throw new IllegalStateException("TracyBindings has no native methods");
            }
        });
        java.util.Set<String> seen = new java.util.HashSet<>();
        int[] lowered = new int[1];
        java.util.Map<String, Integer> kinds = new java.util.TreeMap<>();
        int written = 0;
        for (ZipFile zip : jars) {
            for (java.util.Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements();) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (!n.endsWith(".class") || n.startsWith("META-INF/") || n.equals("module-info.class")) {
                    continue;
                }
                String name = n.substring(0, n.length() - 6);
                if (!seen.add(name) || Files.exists(out.resolve(n))) {
                    continue;   // first jar wins, as on TeaVM's class path; a class the bench shadows stays shadowed
                }
                ClassNode cls = new ClassNode();
                try (InputStream in = zip.getInputStream(e)) {
                    new ClassReader(in).accept(cls, 0);
                }
                boolean changed = false;
                Patch p = specific.remove(name);
                if (p != null) {
                    p.apply(cls);
                    changed = true;
                    System.out.println("patched " + name + " (from " + Paths.get(zip.getName()).getFileName() + ")");
                }
                if (lower(cls, kinds)) {
                    changed = true;
                }
                if (changed) {
                    ClassWriter w = new ClassWriter(ClassWriter.COMPUTE_MAXS);
                    cls.accept(w);
                    Path dst = out.resolve(n);
                    Files.createDirectories(dst.getParent());
                    Files.write(dst, w.toByteArray());
                    ++written;
                }
            }
        }
        if (!specific.isEmpty()) {
            throw new IllegalStateException("not found in any jar: " + specific.keySet());
        }
        System.out.println("single-threaded lowering: " + kinds + "; " + written + " classes written");
        for (ZipFile z : jars) {
            z.close();
        }
    }

    private static final String BG = "wjteavm/Background";

    /* Rewrite blocking calls to wjteavm.Background; true if anything changed. */
    private static boolean lower(ClassNode cls, java.util.Map<String, Integer> kinds) {
        boolean changed = false;
        for (MethodNode m : cls.methods) {
            for (org.objectweb.asm.tree.AbstractInsnNode insn : m.instructions.toArray()) {
                if (!(insn instanceof MethodInsnNode)) {
                    continue;
                }
                MethodInsnNode call = (MethodInsnNode) insn;
                String target = null;
                String desc = null;
                if (call.getOpcode() == Opcodes.INVOKESTATIC && call.owner.equals("java/lang/Thread")) {
                    if (call.name.equals("yield") && call.desc.equals("()V")) {
                        target = "yieldHint";
                        desc = "()V";
                    } else if (call.name.equals("sleep") && (call.desc.equals("(J)V") || call.desc.equals("(JI)V"))) {
                        target = "sleep";
                        desc = call.desc;
                    }
                } else if (call.getOpcode() == Opcodes.INVOKEVIRTUAL && call.name.equals("wait")
                        && (call.desc.equals("()V") || call.desc.equals("(J)V") || call.desc.equals("(JI)V"))) {
                    target = "waitOn";
                    desc = "(Ljava/lang/Object;" + call.desc.substring(1);
                } else if (call.getOpcode() == Opcodes.INVOKEVIRTUAL && call.name.equals("join")
                        && (call.desc.equals("()V") || call.desc.equals("(J)V") || call.desc.equals("(JI)V"))
                        && isThread(call.owner)) {
                    target = "join";
                    desc = "(Ljava/lang/Thread;" + call.desc.substring(1);
                }
                if (target == null) {
                    continue;
                }
                m.instructions.set(call, new MethodInsnNode(Opcodes.INVOKESTATIC, BG, target, desc, false));
                kinds.merge(call.owner.equals("java/lang/Thread") || target.equals("waitOn") ? target : target,
                        1, Integer::sum);
                changed = true;
            }
        }
        return changed;
    }

    /* join(...) on a Thread receiver: java/lang/Thread or a subclass we can see by name. */
    private static boolean isThread(String owner) {
        return owner.equals("java/lang/Thread") || owner.endsWith("Thread") || owner.contains("Thread$");
    }

    @SuppressWarnings("unused")
    private static void patch(List<ZipFile> jars, Path out, String name, Patch p) throws Exception {
        for (ZipFile zip : jars) {
            ZipEntry e = zip.getEntry(name + ".class");
            if (e == null) {
                continue;
            }
            ClassNode cls = new ClassNode();
            try (InputStream in = zip.getInputStream(e)) {
                new ClassReader(in).accept(cls, 0);
            }
            p.apply(cls);
            ClassWriter w = new ClassWriter(ClassWriter.COMPUTE_MAXS);
            cls.accept(w);
            Path dst = out.resolve(name + ".class");
            Files.createDirectories(dst.getParent());
            Files.write(dst, w.toByteArray());
            System.out.println("patched " + name + " (from " + Paths.get(zip.getName()).getFileName() + ")");
            return;
        }
        throw new IllegalStateException(name + " is in none of the jars");
    }

    private static void returnTrue(ClassNode cls, String name, String desc) {
        for (MethodNode m : cls.methods) {
            if (m.name.equals(name) && m.desc.equals(desc)) {
                InsnList code = new InsnList();
                code.add(new InsnNode(Opcodes.ICONST_1));
                code.add(new InsnNode(Opcodes.IRETURN));
                m.instructions = code;
                m.tryCatchBlocks.clear();
                m.localVariables = null;
                return;
            }
        }
        throw new IllegalStateException(cls.name + "." + name + desc + " not found");
    }

    /* Replace a method's body with "return the default value of its return type". */
    private static void stub(ClassNode cls, String name, String desc) {
        for (MethodNode m : cls.methods) {
            if (!m.name.equals(name) || !m.desc.equals(desc)) {
                continue;
            }
            InsnList code = new InsnList();
            Type ret = Type.getReturnType(desc);
            switch (ret.getSort()) {
                case Type.VOID:
                    code.add(new InsnNode(Opcodes.RETURN));
                    break;
                case Type.BOOLEAN:
                case Type.BYTE:
                case Type.CHAR:
                case Type.SHORT:
                case Type.INT:
                    code.add(new InsnNode(Opcodes.ICONST_0));
                    code.add(new InsnNode(Opcodes.IRETURN));
                    break;
                case Type.LONG:
                    code.add(new InsnNode(Opcodes.LCONST_0));
                    code.add(new InsnNode(Opcodes.LRETURN));
                    break;
                case Type.FLOAT:
                    code.add(new InsnNode(Opcodes.FCONST_0));
                    code.add(new InsnNode(Opcodes.FRETURN));
                    break;
                case Type.DOUBLE:
                    code.add(new InsnNode(Opcodes.DCONST_0));
                    code.add(new InsnNode(Opcodes.DRETURN));
                    break;
                default:
                    code.add(new InsnNode(Opcodes.ACONST_NULL));
                    code.add(new InsnNode(Opcodes.ARETURN));
            }
            m.instructions = code;
            m.tryCatchBlocks.clear();
            m.localVariables = null;
            return;
        }
        throw new IllegalStateException(cls.name + "." + name + desc + " not found");
    }

    private static void jvmProfiler(ClassNode cls) {
        String noOp = "net/minecraft/util/profiling/jfr/JvmProfiler$NoOpProfiler";
        for (MethodNode m : cls.methods) {
            if (!m.name.equals("<clinit>")) {
                continue;
            }
            InsnList code = new InsnList();
            code.add(new TypeInsnNode(Opcodes.NEW, noOp));
            code.add(new InsnNode(Opcodes.DUP));
            code.add(new MethodInsnNode(Opcodes.INVOKESPECIAL, noOp, "<init>", "()V", false));
            code.add(new FieldInsnNode(Opcodes.PUTSTATIC, cls.name, "INSTANCE",
                    "Lnet/minecraft/util/profiling/jfr/JvmProfiler;"));
            code.add(new InsnNode(Opcodes.RETURN));
            m.instructions = code;
            m.tryCatchBlocks.clear();
            m.localVariables = null;
            return;
        }
        throw new IllegalStateException("JvmProfiler has no <clinit>");
    }
}
