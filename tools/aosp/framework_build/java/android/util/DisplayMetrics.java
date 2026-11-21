package android.util;

import android.content.res.FontScaleConverter;

public class DisplayMetrics {
    public static final int DENSITY_LOW = 120;
    public static final int DENSITY_MEDIUM = 160;
    public static final int DENSITY_DEFAULT = DENSITY_MEDIUM;
    public static final int DENSITY_HIGH = 240;
    public static final int DENSITY_XHIGH = 320;
    public static final int DENSITY_XXHIGH = 480;
    public static final int DENSITY_XXXHIGH = 640;

    public int widthPixels, heightPixels;
    public float density, scaledDensity, xdpi, ydpi;
    public int densityDpi;
    public FontScaleConverter fontScaleConverter;

    public DisplayMetrics() { setToDefaults(); }

    public void setToDefaults() {
        widthPixels = heightPixels = 0;
        density = scaledDensity = 1;
        densityDpi = DENSITY_DEFAULT;
        xdpi = ydpi = DENSITY_DEFAULT;
        fontScaleConverter = null;
    }

    public void setTo(DisplayMetrics other) {
        widthPixels = other.widthPixels;
        heightPixels = other.heightPixels;
        density = other.density;
        densityDpi = other.densityDpi;
        scaledDensity = other.scaledDensity;
        xdpi = other.xdpi;
        ydpi = other.ydpi;
        fontScaleConverter = other.fontScaleConverter;
    }
}
