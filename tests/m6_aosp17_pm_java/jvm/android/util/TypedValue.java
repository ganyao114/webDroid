package android.util;

// Typed values read by XmlBlock from the aapt2 fixtures.
public final class TypedValue {
    public static final int TYPE_NULL = 0;
    public static final int TYPE_REFERENCE = 1;
    public static final int TYPE_STRING = 3;
    public static final int TYPE_FLOAT = 4;
    public static final int TYPE_FIRST_INT = 16;
    public static final int TYPE_LAST_INT = 31;

    public static String coerceToString(int type, int data) {
        if (type == TYPE_FIRST_INT) {
            return Integer.toString(data);
        }
        throw new AssertionError("Unexpected manifest resource value type " + type);
    }
}
