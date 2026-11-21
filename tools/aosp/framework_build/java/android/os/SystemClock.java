package android.os;

/** Android uptime in the host input and vsync clock domain. */
public final class SystemClock {
    private SystemClock() {}
    public static long uptimeMillis() { return elapsedRealtimeNanos() / 1_000_000L; }
    public static long elapsedRealtime() { return uptimeMillis(); }
    public static long currentThreadTimeMillis() { return uptimeMillis(); }
    public static native long elapsedRealtimeNanos();
}
