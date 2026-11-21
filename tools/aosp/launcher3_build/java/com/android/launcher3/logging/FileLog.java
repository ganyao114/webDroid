package com.android.launcher3.logging;

import android.util.Log;

public final class FileLog {
    private FileLog() {}

    public static void d(String tag, String message) { Log.d(tag, message); }
    public static void d(String tag, String message, Throwable cause) { Log.d(tag, message, cause); }
    public static void e(String tag, String message) { Log.e(tag, message); }
    public static void e(String tag, String message, Throwable cause) { Log.e(tag, message, cause); }
}
