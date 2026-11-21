package android.text;

import java.util.Objects;

public final class TextUtils {
    public static boolean isEmpty(CharSequence text) {
        return text == null || text.length() == 0;
    }

    public static boolean equals(CharSequence left, CharSequence right) {
        return Objects.equals(left == null ? null : left.toString(),
                right == null ? null : right.toString());
    }

    public static String emptyIfNull(String text) {
        return text == null ? "" : text;
    }
}
