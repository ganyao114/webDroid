package android.widget;

import org.wasmandroid.ui.NativeMath;

/** Bind the math operations used by upstream Scroller and OverScroller. */
final class Math {
    private Math() {}
    static int abs(int value) { return java.lang.Math.abs(value); }
    static float abs(float value) { return java.lang.Math.abs(value); }
    static int min(int a, int b) { return java.lang.Math.min(a, b); }
    static int max(int a, int b) { return java.lang.Math.max(a, b); }
    static long min(long a, long b) { return java.lang.Math.min(a, b); }
    static float min(float a, float b) { return java.lang.Math.min(a, b); }
    static float max(float a, float b) { return java.lang.Math.max(a, b); }
    static float signum(float value) { return value > 0 ? 1f : value < 0 ? -1f : value; }
    static int round(float value) { return NativeMath.round(value); }
    static long round(double value) {
        if (value != value) return 0;
        long integral = (long) value;
        double remainder = value - integral;
        if (remainder >= 0.5 && integral != Long.MAX_VALUE) return integral + 1;
        if (remainder < -0.5 && integral != Long.MIN_VALUE) return integral - 1;
        return integral;
    }
    static double sqrt(double value) { return NativeMath.sqrt(value); }
    static double exp(double value) { return NativeMath.exp(value); }
    static double log(double value) { return NativeMath.log(value); }
    static double hypot(double x, double y) { return NativeMath.sqrt(x * x + y * y); }
}
