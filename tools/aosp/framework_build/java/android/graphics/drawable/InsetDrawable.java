package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.util.TypedValue;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParserException;

public class InsetDrawable extends Drawable implements Drawable.Callback {
    private static final int[] INSET_ATTRS = {android.R.attr.inset, android.R.attr.insetLeft,
            android.R.attr.insetTop, android.R.attr.insetRight, android.R.attr.insetBottom};

    private static final class Inset {
        final int pixels;
        final float fraction;
        Inset(int pixels, float fraction) {
            this.pixels = pixels;
            this.fraction = fraction;
        }
        int resolve(int size) { return pixels + (int) (size * fraction); }
    }

    private final Drawable child;
    private final Inset left, top, right, bottom;

    public InsetDrawable(Drawable child, int inset) { this(child, inset, inset, inset, inset); }
    public InsetDrawable(Drawable child, int left, int top, int right, int bottom) {
        this(child, new Inset(left, 0), new Inset(top, 0), new Inset(right, 0), new Inset(bottom, 0));
    }
    public InsetDrawable(Drawable child, float inset) { this(child, inset, inset, inset, inset); }
    public InsetDrawable(Drawable child, float left, float top, float right, float bottom) {
        this(child, new Inset(0, left), new Inset(0, top), new Inset(0, right), new Inset(0, bottom));
    }
    private InsetDrawable(Drawable child, Inset left, Inset top, Inset right, Inset bottom) {
        this.child = child;
        this.left = left; this.top = top; this.right = right; this.bottom = bottom;
        child.setCallback(this);
    }
    public Drawable getDrawable() { return child; }
    @Override public int getIntrinsicWidth() {
        return intrinsicSize(child.getIntrinsicWidth(), left, right);
    }
    @Override public int getIntrinsicHeight() {
        return intrinsicSize(child.getIntrinsicHeight(), top, bottom);
    }
    private static int intrinsicSize(int childSize, Inset start, Inset end) {
        float fraction = start.fraction + end.fraction;
        return childSize < 0 || fraction >= 1 ? -1
                : (int) (childSize / (1 - fraction)) + start.pixels + end.pixels;
    }
    @Override public boolean getPadding(Rect out) {
        child.getPadding(out);
        Rect bounds = getBounds();
        out.left += left.resolve(bounds.width());
        out.right += right.resolve(bounds.width());
        out.top += top.resolve(bounds.height());
        out.bottom += bottom.resolve(bounds.height());
        return (out.left | out.top | out.right | out.bottom) != 0;
    }
    @Override protected void onBoundsChange(Rect b) {
        child.setBounds(b.left + left.resolve(b.width()), b.top + top.resolve(b.height()),
                b.right - right.resolve(b.width()), b.bottom - bottom.resolve(b.height()));
    }
    @Override public void setAlpha(int alpha) { super.setAlpha(alpha); child.setAlpha(alpha); }
    @Override public boolean isStateful() { return child.isStateful(); }
    @Override protected boolean onStateChange(int[] state) { return child.setState(state); }
    @Override public void draw(Canvas canvas) { child.draw(canvas); }
    @Override public void invalidateDrawable(Drawable drawable) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) { scheduleSelf(task, when); }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { unscheduleSelf(task); }

    static InsetDrawable inflateInset(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        try (TypedArray values = theme == null ? res.obtainAttributes(xml, INSET_ATTRS)
                : theme.obtainStyledAttributes(xml, INSET_ATTRS, 0, 0)) {
            Inset all = readInset(values, 0, new Inset(0, 0));
            Inset left = readInset(values, 1, all), top = readInset(values, 2, all);
            Inset right = readInset(values, 3, all), bottom = readInset(values, 4, all);
            return new InsetDrawable(readChild(res, xml, theme), left, top, right, bottom);
        }
    }
    private static Inset readInset(TypedArray values, int index, Inset fallback) throws XmlPullParserException {
        if (!values.hasValue(index)) return fallback;
        if (values.getType(index) == TypedValue.TYPE_FRACTION) {
            float fraction = values.getFraction(index, 1, 1, 0);
            if (fraction >= 1) throw new XmlPullParserException("Inset fraction must be below 1");
            return new Inset(0, fraction);
        }
        return new Inset(values.getDimensionPixelSize(index, 0), 0);
    }
}
