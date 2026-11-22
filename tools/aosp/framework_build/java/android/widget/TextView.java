package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.text.TextPaint;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import java.util.ArrayList;

// Keep the Paint-returning bridge for embedded clients compiled against the older framework DEX.
interface TextViewPaintAccess {
    Paint getPaint();
}

public class TextView extends View implements TextViewPaintAccess {
    public enum BufferType { NORMAL, SPANNABLE, EDITABLE }
    public interface OnEditorActionListener { boolean onEditorAction(TextView view, int actionId, KeyEvent event); }
    private static final int[] TEXT_ATTRS = {android.R.attr.text, android.R.attr.textSize, android.R.attr.textColor,
            android.R.attr.gravity, android.R.attr.singleLine, android.R.attr.maxLines, android.R.attr.minLines,
            android.R.attr.fontFamily, android.R.attr.textStyle, android.R.attr.hint, android.R.attr.textColorHint,
            android.R.attr.includeFontPadding, android.R.attr.textAppearance, android.R.attr.lineSpacingExtra,
            android.R.attr.lineSpacingMultiplier, android.R.attr.lines};
    private final TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final Paint.FontMetrics fontMetrics = new Paint.FontMetrics();
    private final ArrayList<String> lines = new ArrayList<>();
    private CharSequence text = "", hint;
    private ColorStateList colors = ColorStateList.valueOf(Color.BLACK);
    private ColorStateList hintColors = ColorStateList.valueOf(Color.GRAY);
    private int gravity = Gravity.START | Gravity.TOP, maxLines = Integer.MAX_VALUE, minLines;
    private boolean singleLine, includeFontPadding = true;
    private float lineSpacingExtra, lineSpacingMultiplier = 1;
    private int drawablePadding;
    private Drawable start, top, end, bottom;
    private boolean relativeDrawables;
    private TextUtils.TruncateAt ellipsize;
    public TextView(Context context) { this(context, null); }
    public TextView(Context context, AttributeSet attrs) { this(context, attrs, android.R.attr.textViewStyle); }
    public TextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public TextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setTextSize(14);
        try (TypedArray a = context.obtainStyledAttributes(attrs, TEXT_ATTRS, defStyleAttr, defStyleRes)) {
            int appearance = a.getResourceId(12, 0);
            if (appearance != 0) setTextAppearance(appearance);
            applyTextAppearance(a);
            applyText(a.getText(0));
            setHint(a.getText(9));
            setGravity(a.getInt(3, gravity));
            singleLine = a.getBoolean(4, false);
            maxLines = a.getInt(5, maxLines);
            minLines = a.getInt(6, minLines);
            int exactLines = a.getInt(15, -1);
            if (exactLines >= 0) setLines(exactLines);
            lineSpacingExtra = a.getDimension(13, 0);
            lineSpacingMultiplier = a.getFloat(14, 1);
        }
    }
    private void applyTextAppearance(TypedArray a) {
        if (a.hasValue(1)) setTextSize(TypedValue.COMPLEX_UNIT_PX, a.getDimension(1, paint.getTextSize()));
        if (a.hasValue(2)) setTextColor(a.getColorStateList(2));
        if (a.hasValue(10)) setHintTextColor(a.getColorStateList(10));
        if (a.hasValue(11)) includeFontPadding = a.getBoolean(11, true);
        if (a.hasValue(7) || a.hasValue(8)) setTypeface(Typeface.create(a.getString(7), a.getInt(8, Typeface.NORMAL)));
    }
    public void setTextAppearance(int id) {
        try (TypedArray appearance = getContext().obtainStyledAttributes(id, TEXT_ATTRS)) { applyTextAppearance(appearance); }
    }
    public void setTextAppearance(Context context, int id) { setTextAppearance(id); }
    public CharSequence getText() { return text; }
    public void setText(CharSequence value) { setText(value, BufferType.NORMAL); }
    public void setText(CharSequence value, BufferType type) { applyText(value); }
    private void applyText(CharSequence value) {
        text = value == null ? "" : value;
        requestLayout();
        invalidate();
    }
    public final void setText(int id) { setText(getResources().getText(id)); }
    public int length() { return text.length(); }
    public void setHint(CharSequence hint) { this.hint = hint; requestLayout(); invalidate(); }
    public void setHint(int id) { setHint(getResources().getText(id)); }
    public CharSequence getHint() { return hint; }
    public void setTextSize(float size) { setTextSize(TypedValue.COMPLEX_UNIT_SP, size); }
    public void setTextSize(int unit, float size) {
        float pixels = TypedValue.applyDimension(unit, size, getResources().getDisplayMetrics());
        if (pixels == paint.getTextSize()) return;
        paint.setTextSize(pixels);
        requestLayout();
        invalidate();
    }
    public float getTextSize() { return paint.getTextSize(); }
    public void setTextColor(int color) { applyTextColor(ColorStateList.valueOf(color)); }
    public void setTextColor(ColorStateList colors) { applyTextColor(colors); }
    private void applyTextColor(ColorStateList colors) {
        if (colors == null) throw new NullPointerException("colors");
        this.colors = colors;
        invalidate();
    }
    public ColorStateList getTextColors() { return colors; }
    public int getCurrentTextColor() { return colors.getColorForState(getDrawableState(), colors.getDefaultColor()); }
    public void setHintTextColor(int color) { setHintTextColor(ColorStateList.valueOf(color)); }
    public void setHintTextColor(ColorStateList color) { if (color != null) { hintColors = color; invalidate(); } }
    public ColorStateList getHintTextColors() { return hintColors; }
    public int getCurrentHintTextColor() { return hintColors.getColorForState(getDrawableState(), hintColors.getDefaultColor()); }
    @Override public TextPaint getPaint() { return paint; }
    public void setShadowLayer(float radius, float dx, float dy, int color) {
        paint.setShadowLayer(radius, dx, dy, color);
        invalidate();
    }
    public float getShadowRadius() { return paint.getShadowLayerRadius(); }
    public float getShadowDx() { return paint.getShadowLayerDx(); }
    public float getShadowDy() { return paint.getShadowLayerDy(); }
    public int getShadowColor() { return paint.getShadowLayerColor(); }
    public int getExtendedPaddingTop() { return getCompoundPaddingTop(); }
    public int getExtendedPaddingBottom() { return getCompoundPaddingBottom(); }
    public float getLetterSpacing() { return paint.getLetterSpacing(); }
    public void setLetterSpacing(float spacing) {
        if (Float.compare(paint.getLetterSpacing(), spacing) == 0) return;
        paint.setLetterSpacing(spacing);
        requestLayout();
        invalidate();
    }
    public void setTypeface(Typeface font) { paint.setTypeface(font); requestLayout(); invalidate(); }
    public void setTypeface(Typeface font, int style) { setTypeface(Typeface.create(font, style)); }
    public Typeface getTypeface() { return paint.getTypeface(); }
    public void setGravity(int gravity) {
        if ((gravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) == 0) gravity |= Gravity.START;
        if ((gravity & Gravity.VERTICAL_GRAVITY_MASK) == 0) gravity |= Gravity.TOP;
        this.gravity = gravity;
        requestLayout();
        invalidate();
    }
    public int getGravity() { return gravity; }
    public void setSingleLine() { setSingleLine(true); }
    public void setSingleLine(boolean value) { singleLine = value; requestLayout(); }
    public void setMaxLines(int lines) { maxLines = Math.max(0, lines); requestLayout(); }
    public void setMinLines(int lines) { minLines = Math.max(0, lines); requestLayout(); }
    public void setLines(int lines) { setMinLines(lines); setMaxLines(lines); }
    public int getMaxLines() { return singleLine ? 1 : maxLines; }
    public int getMinLines() { return minLines; }
    public int getLineCount() { return lines.size(); }
    public void setIncludeFontPadding(boolean flag) { includeFontPadding = flag; requestLayout(); }
    public boolean getIncludeFontPadding() { return includeFontPadding; }
    public void setLineSpacing(float extra, float multiplier) { lineSpacingExtra = extra; lineSpacingMultiplier = multiplier; requestLayout(); }
    public int getLineHeight() {
        paint.getFontMetrics(fontMetrics);
        return lineAdvance();
    }
    private int lineAdvance() {
        float spacing = fontMetrics.descent - fontMetrics.ascent + fontMetrics.leading;
        return Math.round(spacing * lineSpacingMultiplier + lineSpacingExtra);
    }
    private float firstLineTop() { return includeFontPadding ? fontMetrics.top : fontMetrics.ascent; }
    private float paragraphHeight(int count) {
        if (count == 0) return 0;
        float lastLineBottom = includeFontPadding ? fontMetrics.bottom : fontMetrics.descent;
        float height = lastLineBottom - firstLineTop();
        return count == 1 ? height : height + (count - 1) * lineAdvance();
    }
    public void setEllipsize(TextUtils.TruncateAt where) { ellipsize = where; requestLayout(); invalidate(); }
    public TextUtils.TruncateAt getEllipsize() { return ellipsize; }
    public void setCompoundDrawablePadding(int padding) {
        if (padding != drawablePadding) { drawablePadding = padding; requestLayout(); invalidate(); }
    }
    public int getCompoundDrawablePadding() { return drawablePadding; }
    public void setCompoundDrawablesRelative(Drawable start, Drawable top, Drawable end, Drawable bottom) {
        relativeDrawables = true;
        applyCompoundDrawables(start, top, end, bottom);
    }
    public void setCompoundDrawables(Drawable left, Drawable top, Drawable right, Drawable bottom) {
        relativeDrawables = false;
        applyCompoundDrawables(left, top, right, bottom);
    }
    private void applyCompoundDrawables(Drawable start, Drawable top, Drawable end, Drawable bottom) {
        if (this.start != null) this.start.setCallback(null);
        if (this.top != null) this.top.setCallback(null);
        if (this.end != null) this.end.setCallback(null);
        if (this.bottom != null) this.bottom.setCallback(null);
        this.start = start; this.top = top; this.end = end; this.bottom = bottom;
        if (start != null) start.setCallback(this);
        if (top != null) top.setCallback(this);
        if (end != null) end.setCallback(this);
        if (bottom != null) bottom.setCallback(this);
        requestLayout();
        invalidate();
    }
    private Drawable leftDrawable() { return relativeDrawables && getLayoutDirection() == LAYOUT_DIRECTION_RTL ? end : start; }
    private Drawable rightDrawable() { return relativeDrawables && getLayoutDirection() == LAYOUT_DIRECTION_RTL ? start : end; }
    public Drawable[] getCompoundDrawables() { return new Drawable[] {leftDrawable(), top, rightDrawable(), bottom}; }
    public Drawable[] getCompoundDrawablesRelative() {
        if (relativeDrawables || getLayoutDirection() != LAYOUT_DIRECTION_RTL) return new Drawable[] {start, top, end, bottom};
        return new Drawable[] {end, top, start, bottom};
    }
    private int drawableWidth(Drawable drawable) { return drawable == null ? 0 : drawable.getBounds().width() + drawablePadding; }
    private int drawableHeight(Drawable drawable) {
        return drawable == null ? 0 : drawable.getBounds().height() + (text.length() == 0 && hint == null ? 0 : drawablePadding);
    }
    public int getCompoundPaddingLeft() { return getPaddingLeft() + drawableWidth(leftDrawable()); }
    public int getCompoundPaddingTop() { return getPaddingTop() + drawableHeight(top); }
    public int getCompoundPaddingRight() { return getPaddingRight() + drawableWidth(rightDrawable()); }
    public int getCompoundPaddingBottom() { return getPaddingBottom() + drawableHeight(bottom); }

    private String shownText() {
        String source = text.length() == 0 && hint != null ? hint.toString() : text.toString();
        if (singleLine && (source.indexOf('\n') >= 0 || source.indexOf('\r') >= 0)) {
            StringBuilder result = new StringBuilder(source.length());
            for (int i = 0; i < source.length(); ++i) {
                char c = source.charAt(i);
                result.append(c == '\n' || c == '\r' ? ' ' : c);
            }
            return result.toString();
        }
        return source;
    }
    private void wrap(String source, float width) {
        lines.clear();
        int count = source.length(), start = 0, limit = getMaxLines();
        while (count > 0 && start <= count && lines.size() < limit) {
            int lineEnd = start;
            while (lineEnd < count && source.charAt(lineEnd) != '\n') lineEnd++;
            boolean fits = start == lineEnd || paint.measureText(source, start, lineEnd) <= width;
            boolean truncate = ellipsize == TextUtils.TruncateAt.END && (singleLine || lines.size() + 1 == limit);
            if (singleLine || fits || truncate) {
                String value = source.substring(start, lineEnd);
                if (truncate && !fits) {
                    value = TextUtils.ellipsize(value, paint, width, ellipsize).toString();
                    if (value.isEmpty()) value = "…";
                }
                lines.add(value);
                start = lineEnd + 1;
                continue;
            }
            int fitted = start, wordEnd = -1;
            while (fitted < lineEnd) {
                int next = fitted + 1;
                char c = source.charAt(fitted);
                if (c >= 0xd800 && c <= 0xdbff && next < lineEnd) next++;
                if (fitted > start && paint.measureText(source, start, next) > width) break;
                fitted = next;
                if (c == ' ') wordEnd = fitted;
            }
            int line = wordEnd > start && fitted < lineEnd ? wordEnd : fitted;
            lines.add(source.substring(start, line));
            start = line == lineEnd ? lineEnd + 1 : line;
        }
    }
    @Override protected void onMeasure(int w, int h) {
        String source = shownText();
        int hp = getCompoundPaddingLeft() + getCompoundPaddingRight();
        int vp = getCompoundPaddingTop() + getCompoundPaddingBottom();
        wrap(source, Float.POSITIVE_INFINITY);
        float maxWidth = 0;
        for (int i = 0; i < lines.size(); i++) maxWidth = Math.max(maxWidth, paint.measureText(lines.get(i)));
        if (top != null) maxWidth = Math.max(maxWidth, top.getBounds().width());
        if (bottom != null) maxWidth = Math.max(maxWidth, bottom.getBounds().width());
        int width = resolveSizeAndState(Math.max(getSuggestedMinimumWidth(), hp + (int) (maxWidth + 0.999f)), w, 0);
        wrap(source, Math.max(0, (width & MEASURED_SIZE_MASK) - hp));
        paint.getFontMetrics(fontMetrics);
        int desiredHeight = Math.max(getSuggestedMinimumHeight(),
                vp + (int) java.lang.Math.ceil(paragraphHeight(Math.max(minLines, lines.size()))));
        setMeasuredDimension(width, resolveSizeAndState(desiredHeight, h, 0));
    }
    private float paragraphTop(float textHeight) {
        float rest = layoutHeight() - getCompoundPaddingTop() - getCompoundPaddingBottom() - textHeight;
        int vertical = gravity & Gravity.VERTICAL_GRAVITY_MASK;
        float shift = vertical == Gravity.BOTTOM ? rest : vertical == Gravity.CENTER_VERTICAL ? rest * 0.5f : 0;
        return getCompoundPaddingTop() + Math.max(0, shift);
    }
    private int layoutHeight() { return getHeight() == 0 ? getMeasuredHeight() : getHeight(); }
    private float firstBaseline(float textTop, int lineAdvance) {
        float baseline = textTop - firstLineTop();
        if (includeFontPadding && !lines.isEmpty()) {
            // An exact height can fit the glyphs while leaving less room for font padding.
            float lastGlyphBottom = baseline + (lines.size() - 1) * lineAdvance + fontMetrics.descent;
            float overflow = lastGlyphBottom - (layoutHeight() - getCompoundPaddingBottom());
            if (overflow > 0) baseline -= Math.min(overflow, fontMetrics.ascent - fontMetrics.top);
        }
        return baseline;
    }
    @Override public int getBaseline() {
        paint.getFontMetrics(fontMetrics);
        return Math.round(firstBaseline(paragraphTop(paragraphHeight(lines.size())),
                lines.size() > 1 ? lineAdvance() : 0));
    }
    @Override protected void onDraw(Canvas canvas) {
        paint.getFontMetrics(fontMetrics);
        float textHeight = paragraphHeight(lines.size());
        float textTop = paragraphTop(textHeight);
        Drawable left = leftDrawable(), right = rightDrawable();
        if (left != null) drawCompound(canvas, left, getPaddingLeft(), (getHeight() - left.getBounds().height()) * 0.5f);
        if (right != null) drawCompound(canvas, right, getWidth() - getPaddingRight() - right.getBounds().width(),
                (getHeight() - right.getBounds().height()) * 0.5f);
        if (top != null) drawCompound(canvas, top, (getWidth() - top.getBounds().width()) * 0.5f,
                textTop - drawableHeight(top));
        if (bottom != null) drawCompound(canvas, bottom, (getWidth() - bottom.getBounds().width()) * 0.5f,
                textTop + textHeight + drawablePadding);
        if (lines.isEmpty()) return;
        paint.setColor(text.length() == 0 && hint != null ? getCurrentHintTextColor() : getCurrentTextColor());
        float x;
        switch (Gravity.getAbsoluteGravity(gravity, getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK) {
            case Gravity.RIGHT: x = getWidth() - getCompoundPaddingRight(); paint.setTextAlign(Paint.Align.RIGHT); break;
            case Gravity.CENTER_HORIZONTAL:
                x = (getWidth() + getCompoundPaddingLeft() - getCompoundPaddingRight()) * 0.5f;
                paint.setTextAlign(Paint.Align.CENTER); break;
            default: x = getCompoundPaddingLeft(); paint.setTextAlign(Paint.Align.LEFT); break;
        }
        int advance = lines.size() > 1 ? lineAdvance() : 0;
        float y = firstBaseline(textTop, advance);
        int state = canvas.save();
        try {
            canvas.clipRect(getCompoundPaddingLeft(), getCompoundPaddingTop(), getWidth() - getCompoundPaddingRight(),
                    getHeight() - getCompoundPaddingBottom());
            for (int i = 0; i < lines.size(); i++) { canvas.drawText(lines.get(i), x, y, paint); y += advance; }
        } finally {
            canvas.restoreToCount(state);
        }
    }
    private static void drawCompound(Canvas canvas, Drawable drawable, float x, float y) {
        Rect bounds = drawable.getBounds();
        int state = canvas.save();
        try {
            canvas.translate(x - bounds.left, y - bounds.top);
            drawable.draw(canvas);
        } finally {
            canvas.restoreToCount(state);
        }
    }
}
