// Compile-time declaration for a hidden AOSP API.
package android.os;

public final class ServiceManager {
    public static native IBinder getService(String name);
}
