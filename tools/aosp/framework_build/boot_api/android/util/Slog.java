package android.util;

/** Compile-time declaration for the logger supplied by the interpreter. */
final class Slog {
    static native int wtf(String tag, String message);
}
