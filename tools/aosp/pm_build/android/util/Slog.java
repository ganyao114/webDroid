// Compile-time declaration for a hidden AOSP API.
package android.util;

public final class Slog {
    public static native int i(String tag, String message);
    public static native int w(String tag, String message);
    public static native int w(String tag, String message, Throwable cause);
    public static native int wtf(String tag, String message);
    public static native int wtf(String tag, String message, Throwable cause);
}
