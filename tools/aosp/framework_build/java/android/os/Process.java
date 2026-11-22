package android.os;

public final class Process {
    public static final int INVALID_UID = -1;
    public static final int THREAD_PRIORITY_DEFAULT = 0;
    public static final int THREAD_PRIORITY_FOREGROUND = -2;
    public static final int THREAD_PRIORITY_BACKGROUND = 10;

    private Process() {}
    public static UserHandle myUserHandle() { return UserHandle.SYSTEM; }
    public static int myUid() { return 10000; }
    public static int myTid() {
        Looper looper = Looper.myLooper();
        return (int) (looper == null ? Thread.currentThread() : looper.getThread()).getId();
    }

    // Guest loopers share the host thread and use the same frame scheduling.
    public static void setThreadPriority(int priority) {}
    public static void setThreadPriority(int tid, int priority) {}
}
