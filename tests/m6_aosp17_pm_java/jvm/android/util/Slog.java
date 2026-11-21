package android.util;

public final class Slog {
    public static int w(String tag, String message) {
        System.err.println(tag + " " + message);
        return 0;
    }

    public static int w(String tag, String message, Throwable cause) {
        cause.printStackTrace(System.err);
        return w(tag, message);
    }

    public static int wtf(String tag, String message, Throwable cause) {
        throw new AssertionError(tag + " " + message, cause);
    }
}
