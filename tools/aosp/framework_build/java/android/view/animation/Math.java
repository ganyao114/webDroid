package android.view.animation;

import org.wasmandroid.ui.NativeMath;

final class Math {
    static final double PI = java.lang.Math.PI;
    private Math() {}
    static double cos(double x) { return NativeMath.cos(x); }
    static double pow(double base, double exponent) { return NativeMath.pow(base, exponent); }
}
