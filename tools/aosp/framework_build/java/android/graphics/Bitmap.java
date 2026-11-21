package android.graphics;

public final class Bitmap {
    public static final int DENSITY_NONE = 0;
    public enum Config { ALPHA_8, RGB_565, ARGB_4444, ARGB_8888, RGBA_F16, HARDWARE, RGBA_1010102 }

    final int width, height;
    final long identity = nextIdentity++;
    int generation = 1;
    int[] pixels;
    private static long nextIdentity = 1;
    private final Config config;
    private final boolean mutable;
    private boolean hasAlpha = true;
    private int density = 160;

    Bitmap(int width, int height, Config config, int[] pixels, boolean mutable) {
        this.width = width; this.height = height; this.config = config;
        this.pixels = pixels; this.mutable = mutable;
        hasAlpha = config != Config.RGB_565;
    }

    public static Bitmap createBitmap(int width, int height, Config config) {
        return createBitmap(width, height, config, config != Config.RGB_565);
    }
    public static Bitmap createBitmap(int width, int height, Config config, boolean hasAlpha) {
        if (config == null) throw new NullPointerException("config");
        if (config == Config.HARDWARE) throw new IllegalArgumentException("Cannot create mutable hardware bitmap");
        Bitmap bitmap = new Bitmap(width, height, config, nativeAllocate(width, height), true);
        if (!hasAlpha || config == Config.RGB_565) {
            bitmap.setHasAlpha(false);
            bitmap.eraseColor(Color.BLACK);
        }
        return bitmap;
    }
    public static Bitmap createBitmap(int[] colors, int width, int height, Config config) {
        return createBitmap(colors, 0, width, width, height, config);
    }
    public static Bitmap createBitmap(int[] colors, int offset, int stride, int width, int height, Config config) {
        Bitmap bitmap = createBitmap(width, height, config);
        bitmap.setPixels(colors, offset, stride, 0, 0, width, height);
        return new Bitmap(width, height, config, bitmap.pixels, false);
    }
    public static Bitmap createBitmap(Bitmap source) {
        if (!source.mutable) return source;
        return source.copy(source.config, false);
    }
    public Bitmap copy(Config config, boolean mutable) {
        Bitmap copy = createBitmap(width, height, config, hasAlpha);
        copy.setPixels(requirePixels(), 0, width, 0, 0, width, height);
        Bitmap result = new Bitmap(width, height, config, copy.pixels, mutable);
        result.density = density;
        result.hasAlpha = copy.hasAlpha;
        return result;
    }
    public static Bitmap createScaledBitmap(Bitmap source, int width, int height, boolean filter) {
        if (width == source.width && height == source.height) return source;
        Bitmap result = createBitmap(width, height, source.config, source.hasAlpha);
        nativeScale(source.requirePixels(), source.width, source.height,
                result.pixels, width, height, filter);
        result.density = source.density;
        return result;
    }
    int[] requirePixels() {
        if (pixels == null) throw new IllegalStateException("Bitmap is recycled");
        return pixels;
    }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getRowBytes() { return width * 4; }
    public int getByteCount() { return getRowBytes() * height; }
    public int getAllocationByteCount() { return getByteCount(); }
    public int getGenerationId() { return generation; }
    public Config getConfig() { return config; }
    public boolean isMutable() { return mutable; }
    public boolean isRecycled() { return pixels == null; }
    public void recycle() { pixels = null; }
    public boolean hasAlpha() { return hasAlpha; }
    public void setHasAlpha(boolean alpha) {
        if (hasAlpha != alpha && config != Config.RGB_565) {
            hasAlpha = alpha;
            generation++;
        }
    }
    public int getDensity() { return density; }
    public void setDensity(int density) { this.density = density; }
    public int getScaledWidth(int targetDensity) { return scaled(width, targetDensity); }
    public int getScaledHeight(int targetDensity) { return scaled(height, targetDensity); }
    private int scaled(int size, int target) {
        return density == 0 || target == 0 || density == target ? size : (size * target + (density >> 1)) / density;
    }

    private void checkRect(int x, int y, int w, int h) {
        if (x < 0 || y < 0 || w < 0 || h < 0 || x > width - w || y > height - h) {
            throw new IllegalArgumentException("Pixels outside bitmap bounds");
        }
    }
    private void checkRows(int[] colors, int offset, int stride, int w, int h) {
        if (Math.abs((long) stride) < w) throw new IllegalArgumentException("stride shorter than width");
        long last = (long) offset + (long) (h - 1) * stride;
        if (Math.min(last, offset) < 0 || Math.max(last, offset) + w > colors.length) {
            throw new ArrayIndexOutOfBoundsException();
        }
    }
    private int storedColor(int color) {
        if (config == Config.ALPHA_8) return color & 0xff000000;
        if (config == Config.RGB_565) {
            int red = Color.red(color) >> 3, green = Color.green(color) >> 2, blue = Color.blue(color) >> 3;
            return Color.rgb(red << 3 | red >> 2, green << 2 | green >> 4, blue << 3 | blue >> 2);
        }
        return hasAlpha ? color : color | 0xff000000;
    }
    public int getPixel(int x, int y) {
        checkRect(x, y, 1, 1);
        return nativePixel(requirePixels(), y * width + x, identity, generation) | (hasAlpha ? 0 : 0xff000000);
    }
    public void setPixel(int x, int y, int color) {
        checkRect(x, y, 1, 1);
        checkMutable();
        nativeFill(requirePixels(), y * width + x, 1, storedColor(color));
        generation++;
    }
    public void eraseColor(int color) {
        checkMutable();
        nativeFill(requirePixels(), 0, width * height, storedColor(color));
        generation++;
    }
    public void getPixels(int[] colors, int offset, int stride, int x, int y, int w, int h) {
        checkRect(x, y, w, h);
        if (w == 0 || h == 0) return;
        checkRows(colors, offset, stride, w, h);
        nativeCopyRows(requirePixels(), y * width + x, width, colors, offset, stride,
                w, h, hasAlpha ? 0 : 1);
    }
    public void setPixels(int[] colors, int offset, int stride, int x, int y, int w, int h) {
        checkMutable();
        checkRect(x, y, w, h);
        if (w == 0 || h == 0) return;
        checkRows(colors, offset, stride, w, h);
        int format = config == Config.ALPHA_8 ? 2 : config == Config.RGB_565 ? 3 : hasAlpha ? 0 : 1;
        nativeCopyRows(colors, offset, stride, requirePixels(), y * width + x, width, w, h, format);
        generation++;
    }
    private void checkMutable() { if (!mutable) throw new IllegalStateException("Bitmap is immutable"); }

    private static native int[] nativeAllocate(int width, int height);
    private static native int nativePixel(int[] pixels, int index, long identity, int generation);
    private static native void nativeCopyRows(int[] source, int srcOffset, int srcStride, int[] destination,
                                              int dstOffset, int dstStride, int width, int height, int format);
    private static native void nativeFill(int[] pixels, int start, int count, int color);
    private static native void nativeScale(int[] pixels, int width, int height,
                                           int[] out, int outWidth, int outHeight, boolean filter);
}
