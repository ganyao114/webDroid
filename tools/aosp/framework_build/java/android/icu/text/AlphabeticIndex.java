package android.icu.text;

import java.text.Normalizer;
import java.util.ArrayList;
import java.util.Locale;

/** Alphabetic section labels for the scripts supported by the embedded locale profile. */
public final class AlphabeticIndex<V> {
    private final ArrayList<String> labels = new ArrayList<>();

    public AlphabeticIndex(Locale locale) {
        labels.add("");
        addLabels(locale);
    }

    public AlphabeticIndex<V> addLabels(Locale... locales) {
        for (Locale locale : locales) {
            String letters = switch (locale.getLanguage()) {
                case "el" -> "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ";
                case "ru", "uk", "bg" -> "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
                case "ja" -> "あかさたなはまやらわ";
                case "ar" -> "ابتثجحخدذرزسشصضطظعغفقكلمنهوي";
                case "he" -> "אבגדהוזחטיכלמנסעפצקרשת";
                default -> "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            };
            for (int i = 0; i < letters.length(); ++i) {
                String letter = letters.substring(i, i + 1);
                if (!labels.contains(letter)) labels.add(letter);
            }
        }
        return this;
    }

    public ImmutableIndex<V> buildImmutableIndex() {
        return new ImmutableIndex<>(labels.toArray(new String[0]));
    }

    public static final class Bucket<V> {
        private final String label;
        private Bucket(String label) { this.label = label; }
        public String getLabel() { return label; }
    }

    public static final class ImmutableIndex<V> {
        private final String[] labels;
        private ImmutableIndex(String[] labels) { this.labels = labels; }
        public int getBucketIndex(CharSequence label) {
            if (label.length() == 0) return 0;
            String normalized = Normalizer.normalize(label, Normalizer.Form.NFD);
            int initial = Character.toUpperCase(normalized.codePointAt(0));
            for (int i = 1; i < labels.length; ++i) {
                if (labels[i].codePointAt(0) == initial) return i;
            }
            return 0;
        }
        public Bucket<V> getBucket(int index) {
            return index >= 0 && index < labels.length ? new Bucket<>(labels[index]) : null;
        }
    }
}
