package android.view;

import android.content.res.Resources;
import android.graphics.Point;
import android.util.DisplayMetrics;

/** The host surface's current display. */
public final class Display {
    public static final int DEFAULT_DISPLAY = 0;
    private final Resources resources;

    public Display(Resources resources) { this.resources = resources; }

    public int getDisplayId() { return DEFAULT_DISPLAY; }
    public float getRefreshRate() {
        long interval = Choreographer.frameIntervalNanos();
        return interval > 0 ? 1_000_000_000f / interval : 60f;
    }
    public int getRotation() {
        DisplayMetrics metrics = resources.getDisplayMetrics();
        return metrics.widthPixels > metrics.heightPixels ? 1 : 0;
    }
    public void getMetrics(DisplayMetrics outMetrics) {
        outMetrics.setTo(resources.getDisplayMetrics());
    }
    public void getRealMetrics(DisplayMetrics outMetrics) { getMetrics(outMetrics); }
    public void getSize(Point outSize) {
        DisplayMetrics metrics = resources.getDisplayMetrics();
        outSize.set(metrics.widthPixels, metrics.heightPixels);
    }
    public void getRealSize(Point outSize) { getSize(outSize); }
}
