// Compile-time declaration for a hidden SDK-level helper.
package com.android.modules.utils.build;

public final class UnboundedSdkLevel {
    public static native boolean isAtMost(String version);
}
