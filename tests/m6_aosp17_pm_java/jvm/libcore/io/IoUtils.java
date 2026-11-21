package libcore.io;

public final class IoUtils {
    public static void closeQuietly(AutoCloseable value) {
        if (value != null) {
            try {
                value.close();
            } catch (Exception error) {
                throw new AssertionError("Fixture close failed", error);
            }
        }
    }
}
