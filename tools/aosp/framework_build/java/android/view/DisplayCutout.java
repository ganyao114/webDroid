package android.view;

import android.graphics.Insets;
import android.graphics.Rect;

/** Cutout bounds used by window insets and display configuration. */
public final class DisplayCutout {
    private final Insets safeInsets;
    private final Rect[] bounds;

    public DisplayCutout(Insets safeInsets, Rect left, Rect top, Rect right, Rect bottom) {
        this.safeInsets = safeInsets;
        bounds = new Rect[] {copy(left), copy(top), copy(right), copy(bottom)};
    }

    private static Rect copy(Rect bounds) { return bounds == null ? new Rect() : new Rect(bounds); }
    public int getSafeInsetLeft() { return safeInsets.left; }
    public int getSafeInsetTop() { return safeInsets.top; }
    public int getSafeInsetRight() { return safeInsets.right; }
    public int getSafeInsetBottom() { return safeInsets.bottom; }
    public Rect getBoundingRectLeft() { return copy(bounds[0]); }
    public Rect getBoundingRectTop() { return copy(bounds[1]); }
    public Rect getBoundingRectRight() { return copy(bounds[2]); }
    public Rect getBoundingRectBottom() { return copy(bounds[3]); }
    public boolean isEmpty() {
        return Insets.NONE.equals(safeInsets) && bounds[0].isEmpty() && bounds[1].isEmpty()
                && bounds[2].isEmpty() && bounds[3].isEmpty();
    }
}
