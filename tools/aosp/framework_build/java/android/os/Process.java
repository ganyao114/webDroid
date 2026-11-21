package android.os;

public final class Process {
    private Process() {}
    public static UserHandle myUserHandle() { return UserHandle.SYSTEM; }
    public static int myUid() { return 10000; }
}
