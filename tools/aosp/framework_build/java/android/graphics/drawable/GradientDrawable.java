package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

public class GradientDrawable extends Drawable {
    public static final int RECTANGLE = 0, OVAL = 1, LINE = 2, RING = 3;
    public static final int LINEAR_GRADIENT = 0, RADIAL_GRADIENT = 1, SWEEP_GRADIENT = 2;
    public enum Orientation { TOP_BOTTOM, TR_BL, RIGHT_LEFT, BR_TL, BOTTOM_TOP, BL_TR, LEFT_RIGHT, TL_BR }
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int shape, width = -1, height = -1, strokeWidth;
    private ColorStateList fill = ColorStateList.valueOf(0), stroke = ColorStateList.valueOf(0);
    private int[] colors;
    private float radius, angle = 90;
    private Rect padding;

    public GradientDrawable() {}
    public GradientDrawable(Orientation orientation, int[] colors) {
        setOrientation(orientation);
        setColors(colors);
    }
    public void setShape(int shape) { this.shape = shape; invalidateSelf(); }
    public int getShape() { return shape; }
    public void setSize(int width, int height) { this.width = width; this.height = height; invalidateSelf(); }
    public void setColor(int color) { setColor(ColorStateList.valueOf(color)); }
    public void setColor(ColorStateList color) {
        fill = color == null ? ColorStateList.valueOf(0) : color;
        colors = null;
        invalidateSelf();
    }
    public ColorStateList getColor() { return fill; }
    public void setCornerRadius(float r) { radius = Math.max(0, r); invalidateSelf(); }
    public float getCornerRadius() { return radius; }
    public void setStroke(int width, int color) { setStroke(width, ColorStateList.valueOf(color)); }
    public void setStroke(int width, ColorStateList color) {
        strokeWidth = Math.max(0, width);
        stroke = color == null ? ColorStateList.valueOf(0) : color;
        invalidateSelf();
    }
    public void setOrientation(Orientation orientation) {
        angle = 90 + 45 * orientation.ordinal();
        invalidateSelf();
    }
    public void setColors(int[] colors) {
        if (colors != null && colors.length < 2) throw new IllegalArgumentException("A gradient needs at least two colors");
        this.colors = colors == null ? null : colors.clone();
        invalidateSelf();
    }
    public int[] getColors() { return colors == null ? null : colors.clone(); }
    @Override public int getIntrinsicWidth() { return width; }
    @Override public int getIntrinsicHeight() { return height; }
    @Override public boolean getPadding(Rect out) {
        if (padding == null) return super.getPadding(out);
        out.set(padding);
        return true;
    }
    @Override public boolean isStateful() { return fill.isStateful() || stroke.isStateful(); }
    @Override protected boolean onStateChange(int[] state) { if (!isStateful()) return false; invalidateSelf(); return true; }

    @Override public void draw(Canvas canvas) {
        Rect b = getBounds();
        if (b.isEmpty()) return;
        if (colors != null && shape == RECTANGLE) {
            canvas.drawLinearGradient(b, radius, modulateAlpha(colors[0]), modulateAlpha(colors[colors.length - 1]), angle);
        } else if (shape != LINE) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(modulateAlpha(fill.getColorForState(getState(), fill.getDefaultColor())));
            drawShape(canvas, b, 0);
        }
        if (strokeWidth != 0 || shape == LINE) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(modulateAlpha(stroke.getColorForState(getState(), stroke.getDefaultColor())));
            drawShape(canvas, b, strokeWidth * 0.5f);
        }
    }
    private void drawShape(Canvas canvas, Rect b, float inset) {
        float l = b.left + inset, t = b.top + inset, r = b.right - inset, bottom = b.bottom - inset;
        if (shape == OVAL) canvas.drawOval(l, t, r, bottom, paint);
        else if (shape == LINE) canvas.drawLine(l, b.exactCenterY(), r, b.exactCenterY(), paint);
        else if (radius > 0) canvas.drawRoundRect(l, t, r, bottom, radius, radius, paint);
        else canvas.drawRect(l, t, r, bottom, paint);
    }

    static GradientDrawable inflateShape(Resources res, XmlResourceParser xml, Resources.Theme theme)
            throws IOException, XmlPullParserException {
        GradientDrawable result = new GradientDrawable();
        result.setShape(new XmlValues(res, theme, xml).integer("shape", RECTANGLE));
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1) continue;
            XmlValues attrs = new XmlValues(res, theme, xml);
            switch (xml.getName()) {
                case "solid": result.setColor(attrs.color("color", 0)); break;
                case "corners": result.setCornerRadius(attrs.dimension("radius", 0)); break;
                case "stroke": result.setStroke(attrs.pixels("width", 0), attrs.color("color", 0)); break;
                case "size": result.setSize(attrs.pixels("width", -1), attrs.pixels("height", -1)); break;
                case "padding":
                    result.padding = new Rect(attrs.pixels("left", 0), attrs.pixels("top", 0),
                            attrs.pixels("right", 0), attrs.pixels("bottom", 0));
                    break;
                case "gradient":
                    result.colors = new int[] {attrs.color("startColor", 0), attrs.color("endColor", 0)};
                    result.angle = -attrs.number("angle", 0);
                    break;
            }
            XmlValues.skip(xml);
        }
        return result;
    }
}
