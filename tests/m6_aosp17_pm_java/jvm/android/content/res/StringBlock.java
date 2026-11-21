package android.content.res;

/** A view of the upstream native string pool for an unstyled manifest. */
final class StringBlock implements AutoCloseable {
    private long pool;

    StringBlock(long pool, boolean useSparse) {
        this.pool = pool;
    }

    CharSequence getSequence(int index) {
        if (pool == 0) {
            throw new IllegalStateException("Closed string pool");
        }
        return nativeGetString(pool, index);
    }

    @Override
    public void close() {
        // The upstream ApkAssets or XmlBlock owns the native pool.
        pool = 0;
    }

    private static native long nativeCreate(byte[] bytes, int offset, int length);
    private static native int nativeGetSize(long pool);
    private static native String nativeGetString(long pool, int index);
    private static native int[] nativeGetStyle(long pool, int index);
    private static native void nativeDestroy(long pool);
}
