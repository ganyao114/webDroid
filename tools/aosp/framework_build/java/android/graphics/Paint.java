package android.graphics;

import org.wasmandroid.ui.NativeMath;

public class Paint {
    public static final int ANTI_ALIAS_FLAG = 1, FILTER_BITMAP_FLAG = 2, DITHER_FLAG = 4;
    public static final int UNDERLINE_TEXT_FLAG = 8, STRIKE_THRU_TEXT_FLAG = 16, FAKE_BOLD_TEXT_FLAG = 32;
    public static final int LINEAR_TEXT_FLAG = 64, SUBPIXEL_TEXT_FLAG = 128;

    public enum Style { FILL, STROKE, FILL_AND_STROKE }
    public enum Align { LEFT, CENTER, RIGHT }
    public enum Cap { BUTT, ROUND, SQUARE }
    public enum Join { MITER, ROUND, BEVEL }

    public static class FontMetrics {
        public float top, ascent, descent, bottom, leading;
    }
    public static class FontMetricsInt {
        public int top, ascent, descent, bottom, leading;
    }

    private int color, flags;
    private float strokeWidth, strokeMiter, textSize, textScaleX, textSkewX;
    private Style style;
    private Align align;
    private Cap cap;
    private Join join;
    private Typeface typeface;

    public Paint() { this(0); }
    public Paint(int flags) { reset(); this.flags |= flags; }
    public Paint(Paint other) { set(other); }
    public void reset() {
        flags = FILTER_BITMAP_FLAG;
        color = Color.BLACK;
        style = Style.FILL;
        align = Align.LEFT;
        cap = Cap.BUTT;
        join = Join.MITER;
        strokeWidth = 0;
        strokeMiter = 4;
        textSize = 16;
        textScaleX = 1;
        textSkewX = 0;
        typeface = null;
    }
    public void set(Paint other) {
        color = other.color; flags = other.flags; style = other.style; align = other.align;
        cap = other.cap; join = other.join; strokeWidth = other.strokeWidth; strokeMiter = other.strokeMiter;
        textSize = other.textSize; textScaleX = other.textScaleX; textSkewX = other.textSkewX;
        typeface = other.typeface;
    }
    public int getColor() { return color; }
    public void setColor(int color) { this.color = color; }
    public int getAlpha() { return Color.alpha(color); }
    public void setAlpha(int alpha) { color = color & 0xffffff | Math.max(0, Math.min(255, alpha)) << 24; }
    public void setARGB(int a, int r, int g, int b) { setColor(Color.argb(a, r, g, b)); }
    public int getFlags() { return flags; }
    public void setFlags(int flags) { this.flags = flags; }
    private void flag(int flag, boolean enabled) { flags = enabled ? flags | flag : flags & ~flag; }
    public final boolean isAntiAlias() { return (flags & ANTI_ALIAS_FLAG) != 0; }
    public void setAntiAlias(boolean enabled) { flag(ANTI_ALIAS_FLAG, enabled); }
    public final boolean isFilterBitmap() { return (flags & FILTER_BITMAP_FLAG) != 0; }
    public void setFilterBitmap(boolean enabled) { flag(FILTER_BITMAP_FLAG, enabled); }
    public final boolean isDither() { return (flags & DITHER_FLAG) != 0; }
    public void setDither(boolean enabled) { flag(DITHER_FLAG, enabled); }
    public final boolean isFakeBoldText() { return (flags & FAKE_BOLD_TEXT_FLAG) != 0; }
    public void setFakeBoldText(boolean enabled) { flag(FAKE_BOLD_TEXT_FLAG, enabled); }
    public final boolean isUnderlineText() { return (flags & UNDERLINE_TEXT_FLAG) != 0; }
    public void setUnderlineText(boolean enabled) { flag(UNDERLINE_TEXT_FLAG, enabled); }
    public final boolean isStrikeThruText() { return (flags & STRIKE_THRU_TEXT_FLAG) != 0; }
    public void setStrikeThruText(boolean enabled) { flag(STRIKE_THRU_TEXT_FLAG, enabled); }
    public Style getStyle() { return style; }
    public void setStyle(Style style) { this.style = java.util.Objects.requireNonNull(style); }
    public float getStrokeWidth() { return strokeWidth; }
    public void setStrokeWidth(float width) { if (width >= 0) strokeWidth = width; }
    public float getStrokeMiter() { return strokeMiter; }
    public void setStrokeMiter(float miter) { if (miter >= 0) strokeMiter = miter; }
    public Cap getStrokeCap() { return cap; }
    public void setStrokeCap(Cap cap) { this.cap = java.util.Objects.requireNonNull(cap); }
    public Join getStrokeJoin() { return join; }
    public void setStrokeJoin(Join join) { this.join = java.util.Objects.requireNonNull(join); }
    public float getTextSize() { return textSize; }
    public void setTextSize(float size) { if (size >= 0) textSize = size; }
    public float getTextScaleX() { return textScaleX; }
    public void setTextScaleX(float scale) { textScaleX = scale; }
    public float getTextSkewX() { return textSkewX; }
    public void setTextSkewX(float skew) { textSkewX = skew; }
    public Align getTextAlign() { return align; }
    public void setTextAlign(Align align) { this.align = java.util.Objects.requireNonNull(align); }
    public Typeface getTypeface() { return typeface; }
    public Typeface setTypeface(Typeface typeface) { this.typeface = typeface; return typeface; }
    int weight() { return isFakeBoldText() ? 700 : typeface == null ? 400 : typeface.weight; }
    int face() { return typeface == null ? 0 : typeface.face; }

    public float measureText(String text) {
        if (text == null) throw new IllegalArgumentException("text is null");
        return nativeMeasure(text, textSize, weight(), face(), null) * textScaleX;
    }
    public float measureText(String text, int start, int end) { return measureText(text.substring(start, end)); }
    public float measureText(CharSequence text, int start, int end) {
        return measureText(text.subSequence(start, end).toString());
    }
    public float measureText(char[] text, int index, int count) {
        if (text == null || index < 0 || count < 0 || index > text.length - count) throw new IllegalArgumentException();
        StringBuilder value = new StringBuilder();
        for (int i = 0; i < count; ++i) value.append(text[index + i]);
        return measureText(value.toString());
    }
    public FontMetrics getFontMetrics() { FontMetrics result = new FontMetrics(); getFontMetrics(result); return result; }
    public float getFontMetrics(FontMetrics out) {
        float[] m = new float[2];
        nativeMeasure("", textSize, weight(), face(), m);
        if (out != null) {
            out.ascent = out.top = -m[0]; out.descent = out.bottom = m[1]; out.leading = 0;
        }
        return m[0] + m[1];
    }
    public FontMetricsInt getFontMetricsInt() { FontMetricsInt m = new FontMetricsInt(); getFontMetricsInt(m); return m; }
    public int getFontMetricsInt(FontMetricsInt out) {
        FontMetrics metrics = getFontMetrics();
        int ascent = NativeMath.round(metrics.ascent);
        int descent = NativeMath.round(metrics.descent);
        if (out != null) {
            out.ascent = out.top = ascent; out.descent = out.bottom = descent; out.leading = 0;
        }
        return descent - ascent;
    }
    public float ascent() { return getFontMetrics().ascent; }
    public float descent() { return getFontMetrics().descent; }
    public float getFontSpacing() { return getFontMetrics(null); }
    public void getTextBounds(String text, int start, int end, Rect out) {
        FontMetricsInt metrics = getFontMetricsInt();
        out.set(0, metrics.ascent, NativeMath.round(measureText(text, start, end)), metrics.descent);
    }

    private static native float nativeMeasure(String text, float size, int weight, int face, float[] metrics);
}
