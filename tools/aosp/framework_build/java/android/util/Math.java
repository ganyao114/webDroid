package android.util;

import org.wasmandroid.ui.NativeMath;

final class Math {
    private Math() {}
    static int min(int a, int b) { return java.lang.Math.min(a, b); }
    static float abs(float x) { return java.lang.Math.abs(x); }
    static int round(float x) { return NativeMath.round(x); }
}
