package android.graphics.drawable;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.Gravity;

public class BitmapDrawable extends Drawable {
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Bitmap bitmap;
    private int density, gravity = Gravity.FILL;

    public BitmapDrawable() { this(null, null); }
    public BitmapDrawable(Bitmap bitmap) { this(null, bitmap); }
    public BitmapDrawable(Resources resources, Bitmap bitmap) {
        this.bitmap = bitmap;
        density = resources == null ? 160 : resources.getDisplayMetrics().densityDpi;
    }
    public final Bitmap getBitmap() { return bitmap; }
    public final Paint getPaint() { return paint; }
    public void setGravity(int gravity) { this.gravity = gravity; invalidateSelf(); }
    public int getGravity() { return gravity; }
    public void setTargetDensity(int density) { this.density = density == 0 ? 160 : density; invalidateSelf(); }
    public void setTargetDensity(android.util.DisplayMetrics metrics) { setTargetDensity(metrics.densityDpi); }
    public void setTargetDensity(Canvas canvas) { setTargetDensity(canvas.getDensity()); }
    public void setFilterBitmap(boolean filter) { paint.setFilterBitmap(filter); invalidateSelf(); }
    @Override public int getIntrinsicWidth() { return bitmap == null ? -1 : bitmap.getScaledWidth(density); }
    @Override public int getIntrinsicHeight() { return bitmap == null ? -1 : bitmap.getScaledHeight(density); }
    @Override public void draw(Canvas canvas) {
        if (bitmap == null || bitmap.isRecycled()) return;
        Rect destination = new Rect();
        Gravity.apply(gravity, getIntrinsicWidth(), getIntrinsicHeight(), getBounds(), destination);
        paint.setAlpha(getAlpha());
        canvas.drawBitmap(bitmap, null, destination, paint);
    }
}
