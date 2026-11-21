package android.app;

public final class ActivityManager {
    private ActivityManager() {}
    public static boolean isRunningInUserTestHarness() { return false; }
}
