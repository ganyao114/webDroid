package android.graphics;

import org.wasmandroid.ui.NativeMath;

public class Color {
    public static final int BLACK = 0xff000000, DKGRAY = 0xff444444, GRAY = 0xff888888;
    public static final int LTGRAY = 0xffcccccc, WHITE = 0xffffffff, RED = 0xffff0000;
    public static final int GREEN = 0xff00ff00, BLUE = 0xff0000ff, YELLOW = 0xffffff00;
    public static final int CYAN = 0xff00ffff, MAGENTA = 0xffff00ff, TRANSPARENT = 0;

    public static int alpha(int color) { return color >>> 24; }
    public static int red(int color) { return color >> 16 & 255; }
    public static int green(int color) { return color >> 8 & 255; }
    public static int blue(int color) { return color & 255; }
    public static int rgb(int red, int green, int blue) { return argb(255, red, green, blue); }
    public static int argb(int alpha, int red, int green, int blue) {
        return alpha << 24 | red << 16 | green << 8 | blue;
    }
    public static int rgb(float red, float green, float blue) { return argb(1f, red, green, blue); }
    public static int argb(float alpha, float red, float green, float blue) {
        return argb(NativeMath.round(alpha * 255), NativeMath.round(red * 255),
                NativeMath.round(green * 255), NativeMath.round(blue * 255));
    }

    public static int parseColor(String text) {
        if (text.startsWith("#")) {
            long packed = Long.parseLong(text.substring(1), 16);
            if (text.length() == 7) return (int) packed | 0xff000000;
            if (text.length() == 9) return (int) packed;
        } else {
            switch (text.toLowerCase()) {
                case "black": return BLACK;
                case "darkgray": case "darkgrey": return DKGRAY;
                case "gray": case "grey": return GRAY;
                case "lightgray": case "lightgrey": return LTGRAY;
                case "white": return WHITE;
                case "red": return RED;
                case "green": case "lime": return GREEN;
                case "blue": return BLUE;
                case "yellow": return YELLOW;
                case "cyan": case "aqua": return CYAN;
                case "magenta": case "fuchsia": return MAGENTA;
                case "maroon": return 0xff800000;
                case "navy": return 0xff000080;
                case "olive": return 0xff808000;
                case "purple": return 0xff800080;
                case "silver": return 0xffc0c0c0;
                case "teal": return 0xff008080;
            }
        }
        throw new IllegalArgumentException("Unknown color " + text);
    }

    public static float luminance(int color) {
        return (float) (0.2126 * linear(red(color)) + 0.7152 * linear(green(color)) + 0.0722 * linear(blue(color)));
    }

    private static double linear(int channel) {
        double srgb = channel / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : NativeMath.pow((srgb + 0.055) / 1.055, 2.4);
    }

    public static void colorToHSV(int color, float[] hsv) {
        RGBToHSV(red(color), green(color), blue(color), hsv);
    }
    public static void RGBToHSV(int red, int green, int blue, float[] hsv) {
        if (hsv.length < 3) throw new RuntimeException("HSV array must have three elements");
        float r = red / 255f, g = green / 255f, b = blue / 255f;
        float max = Math.max(r, Math.max(g, b));
        float min = Math.min(r, Math.min(g, b));
        float delta = max - min;
        float hue = 0;
        if (delta != 0) {
            if (max == r) hue = (g - b) / delta;
            else if (max == g) hue = 2 + (b - r) / delta;
            else hue = 4 + (r - g) / delta;
        }
        hsv[0] = (hue < 0 ? hue + 6 : hue) * 60;
        hsv[1] = max == 0 ? 0 : delta / max;
        hsv[2] = max;
    }
    public static int HSVToColor(float[] hsv) { return HSVToColor(255, hsv); }
    public static int HSVToColor(int alpha, float[] hsv) {
        if (hsv.length < 3) throw new RuntimeException("HSV array must have three elements");
        float hue = hsv[0] >= 0 && hsv[0] < 360 ? hsv[0] / 60f : 0;
        float s = Math.max(0, Math.min(1, hsv[1])), v = Math.max(0, Math.min(1, hsv[2]));
        int sector = (int) hue;
        float fraction = hue - sector;
        int p = NativeMath.round(255 * v * (1 - s)), q = NativeMath.round(255 * v * (1 - s * fraction));
        int t = NativeMath.round(255 * v * (1 - s * (1 - fraction))), brightness = NativeMath.round(255 * v);
        switch (sector) {
            case 0: return argb(alpha, brightness, t, p);
            case 1: return argb(alpha, q, brightness, p);
            case 2: return argb(alpha, p, brightness, t);
            case 3: return argb(alpha, p, q, brightness);
            case 4: return argb(alpha, t, p, brightness);
            default: return argb(alpha, brightness, p, q);
        }
    }
}
