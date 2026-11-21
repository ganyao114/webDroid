package android.graphics;

import android.content.res.Resources;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;

public final class BitmapFactory {
    private BitmapFactory() {}
    public static class Options {
        public boolean inJustDecodeBounds, inMutable, inScaled = true;
        public int inDensity, inTargetDensity, outWidth = -1, outHeight = -1;
        public Bitmap.Config inPreferredConfig = Bitmap.Config.ARGB_8888;
        public String outMimeType;
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length) {
        return decodeByteArray(data, offset, length, null);
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length, Options opts) {
        if (offset < 0 || length < 0 || offset > data.length - length) throw new ArrayIndexOutOfBoundsException();
        int[] bounds = new int[2];
        int[] pixels = nativeDecodePng(data, offset, length, bounds, opts != null && opts.inJustDecodeBounds);
        if (opts != null) {
            opts.outWidth = bounds[0] == 0 ? -1 : bounds[0];
            opts.outHeight = bounds[1] == 0 ? -1 : bounds[1];
            opts.outMimeType = bounds[0] == 0 ? null : "image/png";
        }
        if (pixels == null) return null;
        Bitmap result = new Bitmap(bounds[0], bounds[1], Bitmap.Config.ARGB_8888, pixels, false);
        if (opts == null) return result;
        if (opts.inDensity != 0) result.setDensity(opts.inDensity);
        if (opts.inScaled && opts.inDensity != 0 && opts.inTargetDensity != 0
                && opts.inDensity != opts.inTargetDensity) {
            result = Bitmap.createScaledBitmap(result, result.getScaledWidth(opts.inTargetDensity),
                    result.getScaledHeight(opts.inTargetDensity), true);
            result.setDensity(opts.inTargetDensity);
        }
        if (opts.inMutable || opts.inPreferredConfig != null && opts.inPreferredConfig != result.getConfig()) {
            result = result.copy(opts.inPreferredConfig == null ? result.getConfig() : opts.inPreferredConfig,
                    opts.inMutable);
        }
        return result;
    }
    public static Bitmap decodeResource(Resources resources, int id) {
        Drawable image = resources.getDrawable(id);
        return image instanceof BitmapDrawable ? ((BitmapDrawable) image).getBitmap() : null;
    }
    private static native int[] nativeDecodePng(byte[] data, int offset, int length, int[] size, boolean boundsOnly);
}
