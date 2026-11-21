package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Path;
import android.graphics.Rect;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

public class AdaptiveIconDrawable extends Drawable implements Drawable.Callback {
    private final Drawable background, foreground, monochrome;
    public AdaptiveIconDrawable(Drawable background, Drawable foreground) { this(background, foreground, null); }
    public AdaptiveIconDrawable(Drawable background, Drawable foreground, Drawable monochrome) {
        this.background = background; this.foreground = foreground; this.monochrome = monochrome;
        if (background != null) background.setCallback(this);
        if (foreground != null) foreground.setCallback(this);
        if (monochrome != null) monochrome.setCallback(this);
    }
    public Drawable getBackground() { return background; }
    public Drawable getForeground() { return foreground; }
    public Drawable getMonochrome() { return monochrome; }
    public static float getExtraInsetFraction() { return 0.25f; }
    public Path getIconMask() {
        Rect b = getBounds();
        Path mask = new Path();
        mask.addOval(b.left, b.top, b.right, b.bottom, Path.Direction.CW);
        return mask;
    }
    @Override protected void onBoundsChange(Rect b) {
        int x = b.width() / 4, y = b.height() / 4;
        if (background != null) background.setBounds(b.left - x, b.top - y, b.right + x, b.bottom + y);
        if (foreground != null) foreground.setBounds(b.left - x, b.top - y, b.right + x, b.bottom + y);
        if (monochrome != null) monochrome.setBounds(b.left - x, b.top - y, b.right + x, b.bottom + y);
    }
    @Override public int getIntrinsicWidth() { return intrinsic(true); }
    @Override public int getIntrinsicHeight() { return intrinsic(false); }
    private int intrinsic(boolean horizontal) {
        int a = background == null ? -1 : horizontal ? background.getIntrinsicWidth() : background.getIntrinsicHeight();
        int b = foreground == null ? -1 : horizontal ? foreground.getIntrinsicWidth() : foreground.getIntrinsicHeight();
        int pixels = Math.max(a, b);
        return pixels < 0 ? -1 : pixels * 2 / 3;
    }
    @Override public void setAlpha(int alpha) {
        super.setAlpha(alpha);
        if (background != null) background.setAlpha(alpha);
        if (foreground != null) foreground.setAlpha(alpha);
    }
    @Override public void draw(Canvas canvas) {
        Rect b = getBounds();
        if (b.isEmpty()) return;
        int save = canvas.save();
        try {
            canvas.clipRoundRect(b.left, b.top, b.right, b.bottom, b.width() * 0.5f, b.height() * 0.5f);
            if (background != null) background.draw(canvas);
            if (foreground != null) foreground.draw(canvas);
        } finally {
            canvas.restoreToCount(save);
        }
    }
    @Override public void invalidateDrawable(Drawable drawable) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) { scheduleSelf(task, when); }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { unscheduleSelf(task); }

    static AdaptiveIconDrawable inflateIcon(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        Drawable background = null, foreground = null, monochrome = null;
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1) continue;
            switch (xml.getName()) {
                case "background": background = readChild(res, xml, theme); break;
                case "foreground": foreground = readChild(res, xml, theme); break;
                case "monochrome": monochrome = readChild(res, xml, theme); break;
                default: org.wasmandroid.ui.XmlValues.skip(xml); break;
            }
        }
        if (background == null || foreground == null) throw new XmlPullParserException("Adaptive icon requires background and foreground");
        return new AdaptiveIconDrawable(background, foreground, monochrome);
    }
}
