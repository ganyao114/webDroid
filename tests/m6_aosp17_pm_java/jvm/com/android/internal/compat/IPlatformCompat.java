package com.android.internal.compat;

import android.content.pm.ApplicationInfo;
import android.content.pm.parsing.result.ParseInput;
import android.os.IBinder;

public interface IPlatformCompat {
    boolean isChangeEnabled(long changeId, ApplicationInfo appInfo);

    final class Stub {
        public static IPlatformCompat asInterface(IBinder binder) {
            return (change, app) -> {
                if (!"org.wasmandroid.pm.fixture".equals(app.packageName)) {
                    throw new AssertionError("Unexpected package in compat query " + app.packageName);
                }
                return change == ParseInput.DeferredError.MISSING_EXPORTED_FLAG
                        && app.targetSdkVersion >= 31;
            };
        }
    }
}
