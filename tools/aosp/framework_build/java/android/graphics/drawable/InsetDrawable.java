package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Rect;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public class InsetDrawable extends Drawable implements Drawable.Callback {
    private final Drawable child;
    private final int left, top, right, bottom;

    public InsetDrawable(Drawable child, int inset) { this(child, inset, inset, inset, inset); }
    public InsetDrawable(Drawable child, int left, int top, int right, int bottom) {
        this.child = child;
        this.left = left; this.top = top; this.right = right; this.bottom = bottom;
        child.setCallback(this);
    }
    public Drawable getDrawable() { return child; }
    @Override public int getIntrinsicWidth() {
        int width = child.getIntrinsicWidth();
        return width < 0 ? width : width + left + right;
    }
    @Override public int getIntrinsicHeight() {
        int height = child.getIntrinsicHeight();
        return height < 0 ? height : height + top + bottom;
    }
    @Override public boolean getPadding(Rect out) {
        child.getPadding(out);
        out.left += left; out.top += top; out.right += right; out.bottom += bottom;
        return (out.left | out.top | out.right | out.bottom) != 0;
    }
    @Override protected void onBoundsChange(Rect b) {
        child.setBounds(b.left + left, b.top + top, b.right - right, b.bottom - bottom);
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
        XmlValues values = new XmlValues(res, theme, xml);
        int all = values.pixels("inset", 0);
        int left = values.pixels("insetLeft", all), top = values.pixels("insetTop", all);
        int right = values.pixels("insetRight", all), bottom = values.pixels("insetBottom", all);
        return new InsetDrawable(readChild(res, xml, theme), left, top, right, bottom);
    }
}
