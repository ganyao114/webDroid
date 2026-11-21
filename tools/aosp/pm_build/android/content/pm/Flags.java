// Compile-time declaration for a hidden AOSP API.
package android.content.pm;

// Names from android/content/pm/flags.aconfig.
public final class Flags {
    public static final String FLAG_SDK_DEPENDENCY_INSTALLER =
            "android.content.pm.sdk_dependency_installer";
    public static final String FLAG_SDK_LIB_INDEPENDENCE =
            "android.content.pm.sdk_lib_independence";

    public static native boolean sdkLibIndependence();
}
