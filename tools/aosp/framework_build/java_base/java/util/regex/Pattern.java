package java.util.regex;

import java.util.ArrayList;

/** The Unicode mark and whitespace expressions used for launcher label search. */
public final class Pattern {
    public static final int UNICODE_CHARACTER_CLASS = 0x100;
    private final boolean marks, unicode;

    private Pattern(String regex, int flags) {
        if (!"\\p{M}+".equals(regex) && !"\\s+".equals(regex))
            throw new IllegalArgumentException("Unsupported regular expression " + regex);
        if ((flags & ~UNICODE_CHARACTER_CLASS) != 0) throw new IllegalArgumentException("Unsupported regex flags");
        marks = "\\p{M}+".equals(regex);
        unicode = (flags & UNICODE_CHARACTER_CLASS) != 0;
    }
    public static Pattern compile(String regex) { return compile(regex, 0); }
    public static Pattern compile(String regex, int flags) { return new Pattern(regex, flags); }
    public Matcher matcher(CharSequence input) { return new Matcher(this, input.toString()); }

    boolean includes(int point) {
        if (marks) {
            int type = Character.getType(point);
            return type >= Character.NON_SPACING_MARK && type <= Character.COMBINING_SPACING_MARK;
        }
        return point >= 0x9 && point <= 0xd || point == ' ' || unicode &&
                (point == 0x85 || point == 0xa0 || point == 0x1680 || point >= 0x2000 && point <= 0x200a
                || point == 0x2028 || point == 0x2029 || point == 0x202f || point == 0x205f || point == 0x3000);
    }
    int runEnd(String value, int start) {
        while (start < value.length()) {
            int point = value.codePointAt(start);
            if (!includes(point)) break;
            start += Character.charCount(point);
        }
        return start;
    }
    public String[] split(CharSequence source) { return split(source, 0); }
    public String[] split(CharSequence source, int limit) {
        String value = source.toString();
        ArrayList<String> parts = new ArrayList<>();
        int start = 0;
        for (int i = 0; i < value.length() && (limit <= 0 || parts.size() < limit - 1);) {
            int end = runEnd(value, i);
            if (end == i) {
                i += Character.charCount(value.codePointAt(i));
            } else {
                parts.add(value.substring(start, i));
                i = start = end;
            }
        }
        if (parts.isEmpty()) return new String[] {value};
        parts.add(value.substring(start));
        if (limit == 0) {
            while (!parts.isEmpty() && parts.get(parts.size() - 1).isEmpty()) parts.remove(parts.size() - 1);
        }
        return parts.toArray(new String[parts.size()]);
    }
}
