package java.text;

import java.util.Comparator;
import java.util.Locale;
import java.util.Objects;

public class Collator implements Comparator<Object>, Cloneable {
    public static final int PRIMARY = 0, SECONDARY = 1, TERTIARY = 2, IDENTICAL = 3;
    public static final int NO_DECOMPOSITION = 0, CANONICAL_DECOMPOSITION = 1, FULL_DECOMPOSITION = 2;

    private final Locale locale;
    private int strength = TERTIARY, decomposition = CANONICAL_DECOMPOSITION;
    protected Collator() { this(Locale.getDefault()); }
    private Collator(Locale locale) { this.locale = Objects.requireNonNull(locale); }

    public static Collator getInstance() { return getInstance(Locale.getDefault()); }
    public static Collator getInstance(Locale locale) { return new Collator(locale); }
    public int getStrength() { return strength; }
    public void setStrength(int strength) {
        if (strength < PRIMARY || strength > IDENTICAL) throw new IllegalArgumentException("strength");
        this.strength = strength;
    }
    public int getDecomposition() { return decomposition; }
    public void setDecomposition(int mode) {
        if (mode < NO_DECOMPOSITION || mode > FULL_DECOMPOSITION) throw new IllegalArgumentException("decomposition");
        decomposition = mode;
    }
    public int compare(String left, String right) {
        if (left.equals(right)) return 0;
        return key(left).compareTo(key(right));
    }
    @Override public int compare(Object left, Object right) { return compare((String) left, (String) right); }
    public boolean equals(String left, String right) { return compare(left, right) == 0; }

    private String key(String text) {
        if (decomposition != NO_DECOMPOSITION)
            text = Normalizer.normalize(text, decomposition == FULL_DECOMPOSITION ? Normalizer.Form.NFKD : Normalizer.Form.NFD);
        if (strength <= SECONDARY) text = text.toLowerCase(locale);
        if (strength != PRIMARY) return text;
        StringBuilder key = new StringBuilder(text.length());
        for (int i = 0; i < text.length();) {
            int point = text.codePointAt(i);
            i += Character.charCount(point);
            int category = Normalizer.characterType(point);
            if (category < 6 || category > 8) key.appendCodePoint(point);
        }
        return key.toString();
    }

    @Override public Object clone() {
        Collator copy = new Collator(locale);
        copy.strength = strength;
        copy.decomposition = decomposition;
        return copy;
    }
}
