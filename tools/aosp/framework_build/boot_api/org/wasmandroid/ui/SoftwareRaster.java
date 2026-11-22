package org.wasmandroid.ui;

public final class SoftwareRaster {
    private SoftwareRaster() {}
    public static native void drawPath(int[] pixels, int width, int height, float[] path,
                                       int verbs, int fillType, float[] transform, int color,
                                       float blurRadius, int blurStyle, boolean alphaOnly,
                                       int gradient, int tileMode, float[] geometry, float[] stops, int[] colors,
                                       int style, float strokeWidth, int cap, int join, float miter);
    public static native void extractAlpha(int[] pixels, int width, int height, int[] output, int radius);
    public static native byte[] encodePng(int[] pixels, int width, int height, boolean hasAlpha);
    public static native void drawBitmap(int[] target, int width, int height, int[] source, int sourceWidth,
                                         int sourceHeight, float[] rectangles, float[] transform,
                                         float opacity, int tint, boolean hasAlpha, boolean alphaSource,
                                         boolean alphaTarget, boolean filter, int mask, float radius);
}
