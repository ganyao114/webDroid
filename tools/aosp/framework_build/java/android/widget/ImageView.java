package android.widget;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;
import org.wasmandroid.ui.NativeMath;

public class ImageView extends View {
    public enum ScaleType { MATRIX, FIT_XY, FIT_START, FIT_CENTER, FIT_END, CENTER, CENTER_CROP, CENTER_INSIDE }
    private Drawable image;
    private ScaleType scaleType = ScaleType.FIT_CENTER;
    private boolean adjustBounds, cropToPadding;
    private int maxWidth = Integer.MAX_VALUE, maxHeight = Integer.MAX_VALUE, imageAlpha = 255;

    public ImageView(Context context) { this(context, null); }
    public ImageView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ImageView(Context context, AttributeSet attrs, int defStyle) { this(context, attrs, defStyle, 0); }
    public ImageView(Context context, AttributeSet attrs, int defStyle, int defRes) {
        super(context, attrs, defStyle, defRes);
        if (attrs != null || defStyle != 0 || defRes != 0) {
            try (TypedArray a = context.obtainStyledAttributes(attrs, new int[] {android.R.attr.src, android.R.attr.scaleType,
                    android.R.attr.adjustViewBounds, android.R.attr.cropToPadding, android.R.attr.maxWidth,
                    android.R.attr.maxHeight}, defStyle, defRes)) {
                adjustBounds = a.getBoolean(2, false);
                cropToPadding = a.getBoolean(3, false);
                maxWidth = a.getDimensionPixelSize(4, maxWidth);
                maxHeight = a.getDimensionPixelSize(5, maxHeight);
                int type = a.getInt(1, adjustBounds ? 3 : -1);
                if (type >= 0 && type < ScaleType.values().length) scaleType = ScaleType.values()[type];
                if (a.hasValue(0)) setImageDrawable(a.getDrawable(0));
            }
        }
    }
    public void setImageResource(int id) { setImageDrawable(id == 0 ? null : getContext().getDrawable(id)); }
    public void setImageBitmap(Bitmap bitmap) { setImageDrawable(bitmap == null ? null : new BitmapDrawable(getResources(), bitmap)); }
    public void setImageDrawable(Drawable drawable) {
        if (drawable == image) return;
        if (image != null) image.setCallback(null);
        image = drawable;
        if (image != null) {
            image.setCallback(this);
            image.setAlpha(imageAlpha);
            if (image.isStateful()) image.setState(getDrawableState());
        }
        requestLayout();
        invalidate();
    }
    public Drawable getDrawable() { return image; }
    public void setScaleType(ScaleType type) { if (type == null) throw new NullPointerException(); scaleType = type; requestLayout(); invalidate(); }
    public ScaleType getScaleType() { return scaleType; }
    public void setAdjustViewBounds(boolean value) { adjustBounds = value; if (value) setScaleType(ScaleType.FIT_CENTER); requestLayout(); }
    public boolean getAdjustViewBounds() { return adjustBounds; }
    public void setCropToPadding(boolean value) { cropToPadding = value; invalidate(); }
    public boolean getCropToPadding() { return cropToPadding; }
    public void setMaxWidth(int value) { maxWidth = value; requestLayout(); }
    public int getMaxWidth() { return maxWidth; }
    public void setMaxHeight(int value) { maxHeight = value; requestLayout(); }
    public int getMaxHeight() { return maxHeight; }
    public void setImageAlpha(int alpha) { imageAlpha = Math.max(0, Math.min(255, alpha)); if (image != null) image.setAlpha(imageAlpha); }
    public int getImageAlpha() { return imageAlpha; }
    public void setImageLevel(int level) { if (image != null) image.setLevel(level); }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (image != null && image.isStateful()) image.setState(getDrawableState());
    }

    @Override protected void onMeasure(int w, int h) {
        int iw = image == null ? 0 : Math.max(1, image.getIntrinsicWidth());
        int ih = image == null ? 0 : Math.max(1, image.getIntrinsicHeight());
        int horizontal = getPaddingLeft() + getPaddingRight();
        int vertical = getPaddingTop() + getPaddingBottom();
        int width = resolveSize(Math.min(maxWidth, Math.max(getSuggestedMinimumWidth(), iw + horizontal)), w);
        int height = resolveSize(Math.min(maxHeight, Math.max(getSuggestedMinimumHeight(), ih + vertical)), h);
        if (adjustBounds && iw > 0 && ih > 0) {
            if (MeasureSpec.getMode(w) != MeasureSpec.EXACTLY) {
                width = resolveSize(Math.min(maxWidth, horizontal + NativeMath.round((height - vertical) * iw / (float) ih)), w);
            }
            if (MeasureSpec.getMode(h) != MeasureSpec.EXACTLY) {
                height = resolveSize(Math.min(maxHeight, vertical + NativeMath.round((width - horizontal) * ih / (float) iw)), h);
            }
        }
        setMeasuredDimension(width, height);
    }

    @Override protected void onDraw(Canvas canvas) {
        if (image == null) return;
        int px = getPaddingLeft(), py = getPaddingTop();
        int width = getWidth() - px - getPaddingRight(), height = getHeight() - py - getPaddingBottom();
        int iw = image.getIntrinsicWidth(), ih = image.getIntrinsicHeight();
        if (width <= 0 || height <= 0) return;
        int save = canvas.save();
        try {
            if (cropToPadding || scaleType == ScaleType.CENTER_CROP) canvas.clipRect(px, py, px + width, py + height);
            canvas.translate(px, py);
            if (iw <= 0 || ih <= 0 || scaleType == ScaleType.FIT_XY) {
                image.setBounds(0, 0, width, height);
            } else {
                float sx = (float) width / iw, sy = (float) height / ih, scale = 1f;
                if (scaleType == ScaleType.CENTER_CROP) scale = Math.max(sx, sy);
                else if (scaleType == ScaleType.CENTER_INSIDE) scale = Math.min(1, Math.min(sx, sy));
                else if (scaleType != ScaleType.CENTER && scaleType != ScaleType.MATRIX) scale = Math.min(sx, sy);
                float alignment = scaleType == ScaleType.FIT_END ? 1f
                        : (scaleType == ScaleType.FIT_START || scaleType == ScaleType.MATRIX) ? 0 : 0.5f;
                canvas.translate(alignment * (width - iw * scale), alignment * (height - ih * scale));
                canvas.scale(scale, scale);
                image.setBounds(0, 0, iw, ih);
            }
            image.draw(canvas);
        } finally {
            canvas.restoreToCount(save);
        }
    }
}
