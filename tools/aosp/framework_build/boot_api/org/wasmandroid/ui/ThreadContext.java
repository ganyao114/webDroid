package org.wasmandroid.ui;

/** Interpreter entry point for dispatch on a cooperative Looper. */
public final class ThreadContext {
    private ThreadContext() {}
    public static native Thread swap(Thread thread);
    public static native long uptimeMillis();
    public static native boolean dispatchMainMessage(Object looper, long uptimeMillis);
    public static native void suspendMainLooper();
    public static native void runReadyThreads();
    public static native void yieldWorker();
    public static native boolean postAtTime(Runnable task, long uptimeMillis);
    public static native void removeCallbacks(Runnable task);
    public static native void clearThreadLocals(Thread thread);
    public static native void setParkBlocker(Thread thread, Object blocker);
    public static native Object getParkBlocker(Thread thread);
    public static native void unpark(Thread thread);
    public static native void park(boolean absolute, long time);
}
