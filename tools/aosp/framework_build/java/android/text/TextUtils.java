package android.text;

import java.util.Locale;

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
    public static CharSequence ellipsize(CharSequence text, TextPaint paint, float width, TruncateAt where) {
        int length = text.length();
        if (paint.measureText(text, 0, length) <= width) return text;
        String ellipsis = where == TruncateAt.END_SMALL ? "‥" : "…";
        float remaining = width - paint.measureText(ellipsis);
        if (remaining < 0) return "";
        int prefix = 0, suffix = length;
        if (where == TruncateAt.START || where == TruncateAt.MIDDLE) {
            float suffixWidth = where == TruncateAt.MIDDLE ? remaining / 2 : remaining;
            while (suffix > 0) {
                int start = suffix - Character.charCount(Character.codePointBefore(text, suffix));
                if (paint.measureText(text, start, length) > suffixWidth) break;
                suffix = start;
            }
            remaining -= paint.measureText(text, suffix, length);
        }
        if (where != TruncateAt.START) {
            while (prefix < suffix) {
                int end = prefix + Character.charCount(Character.codePointAt(text, prefix));
                if (paint.measureText(text, 0, end) > remaining) break;
                prefix = end;
            }
        }
        if (prefix == 0 && suffix == length) return "";
        if (text instanceof Spanned) {
            return new SpannableStringBuilder().append(text, 0, prefix).append(ellipsis)
                    .append(text, suffix, length);
        }
        return new StringBuilder().append(text, 0, prefix).append(ellipsis).append(text, suffix, length).toString();
    }
    public static String emptyIfNull(String text) { return text == null ? "" : text; }
    public static int indexOf(CharSequence text, char match) { return indexOf(text, match, 0); }
    public static int indexOf(CharSequence text, char match, int start) {
        for (int i = Math.max(0, start); i < text.length(); i++) if (text.charAt(i) == match) return i;
        return -1;
    }
    public static int lastIndexOf(CharSequence text, char match, int last) {
        for (int i = Math.min(last, text.length() - 1); i >= 0; i--) if (text.charAt(i) == match) return i;
        return -1;
    }
    public static void getChars(CharSequence text, int start, int end, char[] dest, int offset) {
        if (text instanceof GetChars) ((GetChars) text).getChars(start, end, dest, offset);
        else if (text instanceof String) ((String) text).getChars(start, end, dest, offset);
        else for (int i = start; i < end; i++) dest[offset++] = text.charAt(i);
    }
    public static CharSequence toUpperCase(Locale locale, CharSequence source, boolean copySpans) {
        if (locale == null) locale = Locale.getDefault();
        String text = source.toString(), upper = text.toUpperCase(locale);
        if (upper.equals(text)) return source;
        if (!copySpans || !(source instanceof Spanned)) return upper;
        Spanned spans = (Spanned) source;
        SpannableString result = new SpannableString(upper);
        for (Object span : spans.getSpans(0, text.length(), Object.class)) {
            int start = text.substring(0, spans.getSpanStart(span)).toUpperCase(locale).length();
            int end = text.substring(0, spans.getSpanEnd(span)).toUpperCase(locale).length();
            result.setSpan(span, Math.min(start, upper.length()), Math.min(end, upper.length()), spans.getSpanFlags(span));
        }
        return result;
    }
}
