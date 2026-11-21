// Compile-time declaration for a hidden AOSP API.
package android.os;

public final class SystemProperties {
    public static native String get(String name);
    public static native String get(String name, String defaultValue);
}
