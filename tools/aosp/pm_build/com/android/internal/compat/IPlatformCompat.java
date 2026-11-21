// Compile-time declaration for a hidden AOSP API.
package com.android.internal.compat;

import android.content.pm.ApplicationInfo;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

public interface IPlatformCompat extends IInterface {
    boolean isChangeEnabled(long changeId, ApplicationInfo appInfo) throws RemoteException;

    abstract class Stub implements IPlatformCompat {
        public static native IPlatformCompat asInterface(IBinder binder);
    }
}
