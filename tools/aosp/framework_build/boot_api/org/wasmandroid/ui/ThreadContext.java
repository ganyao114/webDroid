package org.wasmandroid.ui;

/** Interpreter entry point for dispatch on a cooperative Looper. */
public final class ThreadContext {
    private ThreadContext() {}
    public static native Thread swap(Thread thread);
}
