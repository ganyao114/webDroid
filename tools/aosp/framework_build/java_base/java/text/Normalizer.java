package java.text;

import java.util.Arrays;

public final class Normalizer {
    public enum Form { NFD, NFC, NFKD, NFKC }

    private static final int S_BASE = 0xac00, L_BASE = 0x1100, V_BASE = 0x1161, T_BASE = 0x11a7;
    private static final int L_COUNT = 19, V_COUNT = 21, T_COUNT = 28;
    private static final int N_COUNT = V_COUNT * T_COUNT, S_COUNT = L_COUNT * N_COUNT;
    private static final int CHUNK = 12000;

    private Normalizer() {}

    public static String normalize(CharSequence input, Form form) {
        if (form == null) throw new NullPointerException("form");
        boolean compatibility = form == Form.NFKD || form == Form.NFKC;
        Buffer points = new Buffer(input.length());
        for (int i = 0; i < input.length();) {
            int point = Character.codePointAt(input, i);
            i += Character.charCount(point);
            decompose(points, point, compatibility);
        }
        if (form == Form.NFC || form == Form.NFKC) points.compose();
        StringBuilder result = new StringBuilder(input.length());
        for (int i = 0; i < points.size; i++) result.appendCodePoint(points.items[i]);
        return result.toString();
    }

    public static boolean isNormalized(CharSequence text, Form form) {
        return normalize(text, form).contentEquals(text);
    }

    static int characterType(int point) { return range(UnicodeData.CATEGORY, point); }
    private static int combining(int point) { return range(UnicodeData.COMBINING, point); }

    private static int records(String[] table, int columns) {
        return ((table.length - 1) * CHUNK + table[table.length - 1].length()) / (2 * columns);
    }

    private static int get(String[] table, int index) {
        int offset = index * 2;
        String segment = table[offset / CHUNK];
        int local = offset % CHUNK;
        return (segment.charAt(local) - 0x800) << 11 | (segment.charAt(local + 1) - 0x800);
    }

    private static int range(String[] table, int point) {
        if (point < 0 || point > 0x10ffff) return 0;
        int low = 0, high = records(table, 2);
        while (low + 1 < high) {
            int mid = (low + high) >>> 1;
            if (get(table, mid * 2) <= point) low = mid;
            else high = mid;
        }
        return get(table, low * 2 + 1);
    }

    private static void decompose(Buffer out, int point, boolean compatibility) {
        if (point >= S_BASE && point < S_BASE + S_COUNT) {
            int syllable = point - S_BASE;
            out.append(L_BASE + syllable / N_COUNT);
            out.append(V_BASE + (syllable % N_COUNT) / T_COUNT);
            if (syllable % T_COUNT != 0) out.append(T_BASE + syllable % T_COUNT);
            return;
        }
        String[] table = UnicodeData.DECOMPOSITIONS;
        int low = 0, high = records(table, 3);
        while (low < high) {
            int mid = (low + high) >>> 1;
            int source = get(table, mid * 3);
            if (point < source) high = mid;
            else if (point > source) low = mid + 1;
            else {
                int length = get(table, mid * 3 + 2);
                if (!compatibility && (length & 32) != 0) break;
                int offset = get(table, mid * 3 + 1);
                for (int i = 0; i < (length & 31); i++)
                    decompose(out, get(UnicodeData.DECOMPOSED_POINTS, offset + i), compatibility);
                return;
            }
        }
        out.append(point);
    }

    private static int composed(int first, int second) {
        if (first >= L_BASE && first < L_BASE + L_COUNT && second >= V_BASE && second < V_BASE + V_COUNT)
            return S_BASE + (first - L_BASE) * N_COUNT + (second - V_BASE) * T_COUNT;
        if (first >= S_BASE && first < S_BASE + S_COUNT && (first - S_BASE) % T_COUNT == 0
                && second > T_BASE && second < T_BASE + T_COUNT) return first + second - T_BASE;
        String[] table = UnicodeData.COMPOSITIONS;
        int low = 0, high = records(table, 3);
        while (low < high) {
            int mid = (low + high) >>> 1;
            int a = get(table, mid * 3), b = get(table, mid * 3 + 1);
            if (first == a && second == b) return get(table, mid * 3 + 2);
            if (first < a || (first == a && second < b)) high = mid;
            else low = mid + 1;
        }
        return -1;
    }

    private static final class Buffer {
        int[] items;
        int size;
        Buffer(int length) { items = new int[Math.max(4, length)]; }
        void append(int point) {
            if (size == items.length) items = Arrays.copyOf(items, size * 2);
            int cls = combining(point), position = size++;
            while (cls != 0 && position > 0 && combining(items[position - 1]) > cls) {
                items[position] = items[position - 1];
                --position;
            }
            items[position] = point;
        }
        void compose() {
            int starter = -1, lastClass = 0, end = 0;
            for (int i = 0; i < size; i++) {
                int point = items[i], cls = combining(point);
                int compound = starter >= 0 && (lastClass < cls || lastClass == 0)
                        ? composed(items[starter], point) : -1;
                if (compound >= 0) {
                    items[starter] = compound;
                } else {
                    if (cls == 0) starter = end;
                    items[end++] = point;
                    lastClass = cls;
                }
            }
            size = end;
        }
    }
}
