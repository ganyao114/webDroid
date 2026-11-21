// Compile-time declaration for a hidden AOSP API.
package libcore.io;

public final class IoUtils {
    public static native void closeQuietly(AutoCloseable closeable);
}
