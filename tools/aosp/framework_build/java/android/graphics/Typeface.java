package android.graphics;

public class Typeface {
    public static final int NORMAL = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3;
    public static final Typeface DEFAULT = new Typeface(0, 400, false);
    public static final Typeface DEFAULT_BOLD = new Typeface(0, 700, false);
    public static final Typeface SANS_SERIF = new Typeface(1, 400, false);
    public static final Typeface SERIF = DEFAULT;
    public static final Typeface MONOSPACE = new Typeface(2, 400, false);

    final int face, weight;
    private final boolean italic;
    private Typeface(int face, int weight, boolean italic) {
        this.face = face; this.weight = weight; this.italic = italic;
    }
    public static Typeface create(String familyName, int style) {
        Typeface base = "monospace".equals(familyName) ? MONOSPACE
                : "sans-serif".equals(familyName) || "Roboto".equals(familyName) ? SANS_SERIF : DEFAULT;
        return create(base, style);
    }
    public static Typeface create(Typeface family, int style) {
        return create(family, (style & BOLD) != 0 ? 700 : 400, (style & ITALIC) != 0);
    }
    public static Typeface create(Typeface family, int weight, boolean italic) {
        if (weight < 1 || weight > 1000) throw new IllegalArgumentException("Invalid font weight");
        return new Typeface(family == null ? 0 : family.face, weight, italic);
    }
    public static Typeface defaultFromStyle(int style) { return create(DEFAULT, style); }
    public int getStyle() { return (weight >= 600 ? BOLD : NORMAL) | (italic ? ITALIC : NORMAL); }
    public int getWeight() { return weight; }
    public boolean isBold() { return (getStyle() & BOLD) != 0; }
    public boolean isItalic() { return italic; }
}
