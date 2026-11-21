package java.util.regex;

public final class Matcher {
    private final Pattern pattern;
    private final String value;
    Matcher(Pattern pattern, String value) { this.pattern = pattern; this.value = value; }

    public String replaceAll(String replacement) {
        if (replacement.indexOf('$') >= 0 || replacement.indexOf('\\') >= 0)
            throw new IllegalArgumentException("Replacement needs literal text");
        StringBuilder output = new StringBuilder(value.length());
        for (int i = 0; i < value.length();) {
            int end = pattern.runEnd(value, i);
            if (end > i) {
                output.append(replacement);
                i = end;
            } else {
                int point = value.codePointAt(i);
                output.appendCodePoint(point);
                i += Character.charCount(point);
            }
        }
        return output.toString();
    }
}
