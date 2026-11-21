package android.text;

public final class TextUtils {
    private TextUtils() {}
    public enum TruncateAt { START, MIDDLE, END, MARQUEE, END_SMALL }

    public static boolean isEmpty(CharSequence text) { return text == null || text.length() == 0; }
    public static boolean equals(CharSequence a, CharSequence b) {
        if (a == b) return true;
        if (a == null || b == null || a.length() != b.length()) return false;
        for (int i = 0; i < a.length(); i++) if (a.charAt(i) != b.charAt(i)) return false;
        return true;
    }
    public static String emptyIfNull(String text) { return text == null ? "" : text; }
    public static int indexOf(CharSequence text, char match) {
        for (int i = 0; i < text.length(); i++) if (text.charAt(i) == match) return i;
        return -1;
    }
}
