package org.wasmandroid.ui;

/** Positional and sequential conversions used by formatted resource strings. */
public final class StringFormat {
    private StringFormat() {}
    public static String format(String format, Object[] args) {
        StringBuilder out = new StringBuilder(format.length());
        int sequential = 0;
        for (int i = 0; i < format.length(); ++i) {
            char c = format.charAt(i);
            if (c != '%') { out.append(c); continue; }
            if (++i >= format.length()) throw new IllegalArgumentException("Missing format conversion");
            c = format.charAt(i);
            if (c == '%') { out.append('%'); continue; }
            if (c == 'n') { out.append('\n'); continue; }
            int start = i, index = 0;
            while (c >= '0' && c <= '9') {
                index = 10 * index + c - '0';
                if (++i >= format.length()) throw new IllegalArgumentException("Missing format conversion");
                c = format.charAt(i);
            }
            if (i > start && c == '$') {
                --index;
                if (++i >= format.length()) throw new IllegalArgumentException("Missing format conversion");
                c = format.charAt(i);
            } else {
                if (i > start) throw new IllegalArgumentException("Unsupported format width");
                index = sequential++;
            }
            if (index < 0 || index >= args.length) throw new IllegalArgumentException("Missing format argument");
            Object arg = args[index];
            switch (c) {
                case 's': out.append(arg); break;
                case 'd': case 'f': out.append(arg); break;
                case 'b': out.append(arg instanceof Boolean ? ((Boolean) arg).booleanValue() : arg != null); break;
                default: throw new IllegalArgumentException("Unsupported format conversion " + c);
            }
        }
        return out.toString();
    }
}
