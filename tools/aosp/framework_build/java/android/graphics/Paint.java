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
    private float[] fontMetricValues;

    private int color, flags;
    private float strokeWidth, strokeMiter, textSize, textScaleX, textSkewX;
    private float letterSpacing;
    private Style style;
    private Align align;
    private Cap cap;
    private Join join;
    private Typeface typeface;
    private MaskFilter maskFilter;
    private Shader shader;
    private float shadowRadius, shadowDx, shadowDy;
    private int shadowColor;

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
        letterSpacing = 0;
        typeface = null;
        maskFilter = null;
        shader = null;
        clearShadowLayer();
    }
    public void set(Paint other) {
        color = other.color; flags = other.flags; style = other.style; align = other.align;
        cap = other.cap; join = other.join; strokeWidth = other.strokeWidth; strokeMiter = other.strokeMiter;
        textSize = other.textSize; textScaleX = other.textScaleX; textSkewX = other.textSkewX;
        letterSpacing = other.letterSpacing;
        typeface = other.typeface;
        maskFilter = other.maskFilter;
        shader = other.shader;
        shadowRadius = other.shadowRadius; shadowDx = other.shadowDx; shadowDy = other.shadowDy;
        shadowColor = other.shadowColor;
    }
    public int getColor() { return color; }
    public MaskFilter getMaskFilter() { return maskFilter; }
    public MaskFilter setMaskFilter(MaskFilter value) { maskFilter = value; return value; }
    public Shader getShader() { return shader; }
    public Shader setShader(Shader value) { shader = value; return value; }
    public void setShadowLayer(float radius, float dx, float dy, int color) {
        shadowRadius = Math.max(0, radius);
        shadowDx = dx;
        shadowDy = dy;
        shadowColor = color;
    }
    public void clearShadowLayer() { setShadowLayer(0, 0, 0, Color.TRANSPARENT); }
    public boolean hasShadowLayer() { return shadowRadius > 0; }
    public float getShadowLayerRadius() { return shadowRadius; }
    public float getShadowLayerDx() { return shadowDx; }
    public float getShadowLayerDy() { return shadowDy; }
    public int getShadowLayerColor() { return shadowColor; }
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
    public float getLetterSpacing() { return letterSpacing; }
    public void setLetterSpacing(float spacing) { letterSpacing = spacing; }
    public Align getTextAlign() { return align; }
    public void setTextAlign(Align align) { this.align = java.util.Objects.requireNonNull(align); }
    public Typeface getTypeface() { return typeface; }
    public Typeface setTypeface(Typeface typeface) { this.typeface = typeface; return typeface; }
    int weight() { return isFakeBoldText() ? 700 : typeface == null ? 400 : typeface.weight; }
    int face() { return typeface == null ? 0 : typeface.face; }

    public float measureText(String text) {
        if (text == null) throw new IllegalArgumentException("text is null");
        float width = nativeMeasure(text, textSize, weight(), face(), null);
        return scaledTextWidth(text, width);
    }
    private float scaledTextWidth(String text, float width) {
        if (letterSpacing == 0 || text.length() == 0) return width * textScaleX;
        int count = 0;
        for (int offset = 0; offset < text.length(); count++) {
            offset += Character.charCount(Character.codePointAt(text, offset));
        }
        return Math.max(0, width + (count - 1) * letterSpacing * textSize) * textScaleX;
    }
    float textScaleForDrawing(String text) {
        if (letterSpacing == 0) return textScaleX;
        // The display list records font runs with an affine transform and no tracking field.
        float width = nativeMeasure(text, textSize, weight(), face(), null);
        return width == 0 ? textScaleX : scaledTextWidth(text, width) / width;
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
        if (fontMetricValues == null) fontMetricValues = new float[2];
        float[] m = fontMetricValues;
        nativeMeasure("", textSize, weight(), face(), m);
        if (out != null) {
            out.ascent = -m[0];
            out.descent = m[1];
            // The browser publishes its font metrics after drawing starts. Reserve a padded
            // line box at first measure so a taller fallback font can fit the same layout.
            out.top = Math.min(out.ascent, -textSize);
            out.bottom = Math.max(out.descent, textSize * 0.25f);
            out.leading = 0;
        }
        return m[0] + m[1];
    }
    public FontMetricsInt getFontMetricsInt() { FontMetricsInt m = new FontMetricsInt(); getFontMetricsInt(m); return m; }
    public float getUnderlineThickness() { return Math.max(1, getTextSize() * 0.05f); }
    public int getFontMetricsInt(FontMetricsInt out) {
        FontMetrics metrics = getFontMetrics();
        int ascent = NativeMath.round(metrics.ascent);
        int descent = NativeMath.round(metrics.descent);
        if (out != null) {
            out.ascent = ascent;
            out.descent = descent;
            out.top = (int) Math.floor(metrics.top);
            out.bottom = (int) Math.ceil(metrics.bottom);
            out.leading = 0;
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
