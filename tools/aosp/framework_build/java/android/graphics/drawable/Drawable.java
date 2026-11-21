package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Rect;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public abstract class Drawable {
    public interface Callback {
        void invalidateDrawable(Drawable drawable);
        void scheduleDrawable(Drawable drawable, Runnable task, long when);
        void unscheduleDrawable(Drawable drawable, Runnable task);
    }

    private final Rect bounds = new Rect();
    private Callback callback;
    private int[] state = new int[0];
    private int level, alpha = 255;
    private ColorFilter colorFilter;
    private boolean visible = true;

    public abstract void draw(Canvas canvas);
    public void setBounds(int l, int t, int r, int b) {
        if (bounds.left != l || bounds.top != t || bounds.right != r || bounds.bottom != b) {
            bounds.set(l, t, r, b);
            onBoundsChange(bounds);
        }
    }
    public void setBounds(Rect rect) { setBounds(rect.left, rect.top, rect.right, rect.bottom); }
    public final Rect getBounds() { return bounds; }
    public final Rect copyBounds() { return new Rect(bounds); }
    public final void copyBounds(Rect out) { out.set(bounds); }
    protected void onBoundsChange(Rect bounds) {}

    public void setAlpha(int alpha) {
        int value = Math.max(0, Math.min(255, alpha));
        if (this.alpha != value) { this.alpha = value; invalidateSelf(); }
    }
    public int getAlpha() { return alpha; }
    protected final int modulateAlpha(int color) {
        return (color & 0xffffff) | ((((color >>> 24) * getAlpha() + 127) / 255) << 24);
    }
    public void setColorFilter(ColorFilter filter) { colorFilter = filter; invalidateSelf(); }
    public ColorFilter getColorFilter() { return colorFilter; }
    public void clearColorFilter() { setColorFilter(null); }
    public int getOpacity() { return -3; }
    public int getIntrinsicWidth() { return -1; }
    public int getIntrinsicHeight() { return -1; }
    public int getMinimumWidth() { return Math.max(0, getIntrinsicWidth()); }
    public int getMinimumHeight() { return Math.max(0, getIntrinsicHeight()); }
    public boolean getPadding(Rect out) { out.setEmpty(); return false; }

    public final void setCallback(Callback callback) { this.callback = callback; }
    public Callback getCallback() { return callback; }
    public void invalidateSelf() { if (callback != null) callback.invalidateDrawable(this); }
    public void scheduleSelf(Runnable task, long when) { if (callback != null) callback.scheduleDrawable(this, task, when); }
    public void unscheduleSelf(Runnable task) { if (callback != null) callback.unscheduleDrawable(this, task); }
    public boolean setVisible(boolean visible, boolean restart) {
        if (this.visible == visible) return false;
        this.visible = visible;
        invalidateSelf();
        return true;
    }
    public final boolean isVisible() { return visible; }
    public boolean isStateful() { return false; }
    public boolean setState(int[] stateSet) {
        if (same(state, stateSet)) return false;
        state = stateSet;
        return onStateChange(stateSet);
    }
    private static boolean same(int[] a, int[] b) {
        if (a == b) return true;
        if (a == null || b == null || a.length != b.length) return false;
        for (int i = 0; i < a.length; ++i) if (a[i] != b[i]) return false;
        return true;
    }
    public int[] getState() { return state; }
    protected boolean onStateChange(int[] states) { return false; }
    public final int getLevel() { return level; }
    public final boolean setLevel(int level) {
        if (this.level == level) return false;
        this.level = level;
        return onLevelChange(level);
    }
    protected boolean onLevelChange(int level) { return false; }
    public Drawable mutate() { return this; }
    public Drawable getCurrent() { return this; }
    public ConstantState getConstantState() { return null; }
    public abstract static class ConstantState {
        public abstract Drawable newDrawable();
        public Drawable newDrawable(Resources resources) { return newDrawable(); }
        public Drawable newDrawable(Resources resources, Resources.Theme theme) { return newDrawable(resources); }
        public abstract int getChangingConfigurations();
    }

    public static Drawable createFromXml(Resources resources, XmlPullParser parser)
            throws IOException, XmlPullParserException {
        return createFromXml(resources, parser, null);
    }

    public static Drawable createFromXml(Resources resources, XmlPullParser parser, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        XmlValues.toStartTag(parser);
        if (!(parser instanceof XmlResourceParser)) throw new XmlPullParserException("Expected compiled drawable XML");
        return inflateElement(resources, (XmlResourceParser) parser, theme);
    }

    static Drawable inflateElement(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        switch (xml.getName()) {
            case "vector": return VectorDrawable.inflateVector(res, xml, theme);
            case "adaptive-icon": return AdaptiveIconDrawable.inflateIcon(res, xml, theme);
            case "layer-list": return LayerDrawable.inflateLayers(res, xml, theme);
            case "shape": return GradientDrawable.inflateShape(res, xml, theme);
            case "color": {
                XmlValues attrs = new XmlValues(res, theme, xml);
                int color = attrs.color("color", 0);
                XmlValues.skip(xml);
                return new ColorDrawable(color);
            }
            case "bitmap": {
                XmlValues attrs = new XmlValues(res, theme, xml);
                Drawable drawable = attrs.drawable("src");
                if (!(drawable instanceof BitmapDrawable)) throw new XmlPullParserException("Missing bitmap src");
                ((BitmapDrawable) drawable).setGravity(attrs.integer("gravity", android.view.Gravity.FILL));
                XmlValues.skip(xml);
                return drawable;
            }
            case "selector": return StateListDrawable.inflateSelector(res, xml, theme);
            default: throw new XmlPullParserException("Unsupported drawable tag " + xml.getName());
        }
    }

    static Drawable readChild(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        Drawable drawable = new XmlValues(res, theme, xml).drawable("drawable");
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event == XmlPullParser.START_TAG && xml.getDepth() == depth + 1) {
                if (drawable != null) throw new XmlPullParserException("Multiple item drawables");
                drawable = inflateElement(res, xml, theme);
            }
        }
        if (drawable == null) throw new XmlPullParserException("Missing item drawable");
        return drawable;
    }
}
