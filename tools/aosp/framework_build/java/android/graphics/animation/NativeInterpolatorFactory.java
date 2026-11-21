package android.graphics.animation;

public final class NativeInterpolatorFactory {
    private NativeInterpolatorFactory() {}
    public static long createLinearInterpolator() { throw noRenderThread(); }
    public static long createDecelerateInterpolator(float factor) { throw noRenderThread(); }
    public static long createAccelerateDecelerateInterpolator() { throw noRenderThread(); }
    private static UnsupportedOperationException noRenderThread() {
        return new UnsupportedOperationException("RenderThread interpolators are unavailable");
    }
}
