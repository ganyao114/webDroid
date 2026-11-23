import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalDateTime;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

import jdk.internal.org.objectweb.asm.ClassReader;
import jdk.internal.org.objectweb.asm.ClassVisitor;
import jdk.internal.org.objectweb.asm.ClassWriter;
import jdk.internal.org.objectweb.asm.MethodVisitor;
import jdk.internal.org.objectweb.asm.Opcodes;

/** Adapts the pinned AndroidX AAR's calls to Java APIs missing in the embedded runtime. */
public class PreferenceLink {
    private static final String API = "org/wasmandroid/ui/PreferenceRuntime";
    private static final String INFLATER = "androidx/preference/PreferenceInflater";
    private static final String INFLATE_EXCEPTION = "android/view/InflateException";
    private static final String RETHROW =
            "(Ljava/lang/Throwable;Ljava/lang/Throwable;)Ljava/lang/Throwable;";
    private static final Map<String, Integer> REWRITES = Map.of(
            INFLATER + ".class", 9,
            "androidx/preference/PreferenceGroup.class", 1,
            "androidx/preference/Preference.class", 4,
            "androidx/preference/PreferenceCategory.class", 1);

    private static byte[] rewrite(String name, byte[] bytes, int expected) {
        ClassReader reader = new ClassReader(bytes);
        ClassWriter writer = new ClassWriter(reader, 0);
        int[] count = {0};
        reader.accept(new ClassVisitor(Opcodes.ASM8, writer) {
            @Override
            public MethodVisitor visitMethod(int access, String methodName, String descriptor,
                    String signature, String[] exceptions) {
                return new MethodVisitor(Opcodes.ASM8,
                        super.visitMethod(access, methodName, descriptor, signature, exceptions)) {
                    private boolean readingPackageName;

                    @Override
                    public void visitMethodInsn(int opcode, String owner, String method, String desc,
                            boolean isInterface) {
                        if (readingPackageName) {
                            if (!owner.equals("java/lang/Package") || !method.equals("getName")
                                    || !desc.equals("()Ljava/lang/String;")) {
                                throw new IllegalArgumentException("Expected Package.getName in " + name);
                            }
                            readingPackageName = false;
                        } else if (owner.equals("java/lang/Class") && method.equals("getPackage")
                                && desc.equals("()Ljava/lang/Package;")) {
                            super.visitMethodInsn(Opcodes.INVOKESTATIC, API, "packageName",
                                    "(Ljava/lang/Class;)Ljava/lang/String;", false);
                            readingPackageName = true;
                            count[0]++;
                        } else if (owner.equals("java/util/Collections") && method.equals("binarySearch")
                                && desc.equals("(Ljava/util/List;Ljava/lang/Object;)I")) {
                            super.visitMethodInsn(Opcodes.INVOKESTATIC, API, method, desc, false);
                            count[0]++;
                        } else if (owner.equals("androidx/appcompat/content/res/AppCompatResources")
                                && method.equals("getDrawable")
                                && desc.equals("(Landroid/content/Context;I)Landroid/graphics/drawable/Drawable;")) {
                            super.visitMethodInsn(Opcodes.INVOKEVIRTUAL, "android/content/Context", method,
                                    "(I)Landroid/graphics/drawable/Drawable;", false);
                            count[0]++;
                        } else if (owner.equals("android/view/View") && (
                                method.equals("setOnCreateContextMenuListener")
                                        && desc.equals("(Landroid/view/View$OnCreateContextMenuListener;)V")
                                || method.equals("setAccessibilityHeading") && desc.equals("(Z)V"))) {
                            // The embedded view has no context menus or heading node attribute.
                            super.visitInsn(Opcodes.POP2);
                            count[0]++;
                        } else if (name.equals(INFLATER + ".class") && method.equals("initCause")
                                && desc.equals("(Ljava/lang/Throwable;)Ljava/lang/Throwable;")
                                && (owner.equals(INFLATE_EXCEPTION)
                                        || owner.equals("org/xmlpull/v1/XmlPullParserException"))) {
                            super.visitMethodInsn(Opcodes.INVOKESTATIC, INFLATER,
                                    "throwInflationException", RETHROW, false);
                            count[0]++;
                        } else {
                            super.visitMethodInsn(opcode, owner, method, desc, isInterface);
                        }
                    }

                    @Override
                    public void visitEnd() {
                        if (readingPackageName) throw new IllegalArgumentException("Unfinished call in " + name);
                        super.visitEnd();
                    }
                };
            }

            @Override
            public void visitEnd() {
                if (name.equals(INFLATER + ".class")) {
                    // The runtime supports exception constructors with a cause.
                    MethodVisitor code = super.visitMethod(Opcodes.ACC_PRIVATE | Opcodes.ACC_STATIC,
                            "throwInflationException", RETHROW, null, null);
                    code.visitCode();
                    code.visitTypeInsn(Opcodes.NEW, INFLATE_EXCEPTION);
                    code.visitInsn(Opcodes.DUP);
                    code.visitVarInsn(Opcodes.ALOAD, 0);
                    code.visitMethodInsn(Opcodes.INVOKEVIRTUAL, "java/lang/Throwable",
                            "getMessage", "()Ljava/lang/String;", false);
                    code.visitVarInsn(Opcodes.ALOAD, 1);
                    code.visitMethodInsn(Opcodes.INVOKESPECIAL, INFLATE_EXCEPTION, "<init>",
                            "(Ljava/lang/String;Ljava/lang/Throwable;)V", false);
                    code.visitInsn(Opcodes.ATHROW);
                    code.visitMaxs(4, 2);
                    code.visitEnd();
                }
                super.visitEnd();
            }
        }, 0);
        if (count[0] != expected) {
            throw new IllegalArgumentException("Unexpected AndroidX call sites in " + name);
        }
        return writer.toByteArray();
    }

    public static void main(String[] args) throws IOException {
        var pending = new HashMap<>(REWRITES);
        try (var input = new ZipFile(args[0]);
                var output = new ZipOutputStream(Files.newOutputStream(Path.of(args[1])))) {
            var entries = input.entries();
            while (entries.hasMoreElements()) {
                var entry = entries.nextElement();
                var name = entry.getName();
                byte[] bytes;
                try (var contents = input.getInputStream(entry)) {
                    bytes = contents.readAllBytes();
                }
                Integer count = pending.remove(name);
                if (count != null) bytes = rewrite(name, bytes, count);
                var copy = new ZipEntry(name);
                copy.setTimeLocal(LocalDateTime.of(1980, 1, 1, 0, 0));
                output.putNextEntry(copy);
                output.write(bytes);
                output.closeEntry();
            }
        }
        if (!pending.isEmpty()) throw new IllegalArgumentException("Missing classes: " + pending.keySet());
    }
}
