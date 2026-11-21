package org.wasmandroid.ui;

/** Math entry points outside the interpreter's boot library. */
public final class NativeMath {
    private NativeMath() {}
    public static native double cos(double x);
    public static native double sqrt(double x);
    public static native double pow(double x, double exponent);
    public static native double exp(double x);
    public static native double log(double x);
    public static native float parseFloat(String text);

    public static int round(float value) {
        if (value != value) return 0;
        int integral = (int) value;
        float fraction = value - integral;
        if (fraction >= 0.5f && integral != Integer.MAX_VALUE) return integral + 1;
        if (fraction < -0.5f && integral != Integer.MIN_VALUE) return integral - 1;
        return integral;
    }
}
