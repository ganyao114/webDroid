package com.android.launcher3;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.TextView;
import com.android.launcher3.model.data.ItemInfo;

public class BubbleTextView extends TextView {
    private MaskedIcon icon;
    private int requestedSize;

    public BubbleTextView(Context context, AttributeSet attrs) {
        super(context, attrs);
        setGravity(Gravity.CENTER);
        setSingleLine();
        setEllipsize(TextUtils.TruncateAt.END);
        setCompoundDrawablePadding(DeviceProfile.pixels(getResources(), 5));
        setPadding(DeviceProfile.pixels(getResources(), 4), 0,
                DeviceProfile.pixels(getResources(), 4), 0);
        TypedValue ripple = new TypedValue();
        if (context.getTheme().resolveAttribute(android.R.attr.selectableItemBackground, ripple, true)) {
            setBackgroundResource(ripple.resourceId);
        }
    }

    public void bind(ItemInfo item, Drawable drawable, DeviceProfile profile, boolean labels,
                     OnClickListener launch) {
        setTag(item);
        setText(labels ? item.title : null);
        setTextSize(TypedValue.COMPLEX_UNIT_SP, profile.iconTextSp);
        setContentDescription(item.contentDescription != null ? item.contentDescription : item.title);
        setOnClickListener(launch);
        icon = new MaskedIcon(drawable, getContext().getColor(R.color.materialColorSurfaceContainer));
        requestedSize = profile.iconPx;
        updateIcon(getWidth(), getHeight());
    }

    private void updateIcon(int width, int height) {
        int label = TextUtils.isEmpty(getText()) ? 0
                : getMaxLines() * getLineHeight() + getCompoundDrawablePadding();
        int size = width == 0 || height == 0 ? requestedSize : Math.max(1, Math.min(requestedSize,
                Math.min(width - getPaddingLeft() - getPaddingRight(), height - label)));
        if (icon == null || size == icon.getBounds().width()) return;
        icon.setBounds(0, 0, size, size);
        setCompoundDrawablesRelative(null, icon, null, null);
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        updateIcon(width, height);
    }

    private static final class MaskedIcon extends Drawable {
        private final Drawable original;
        private final int backdrop;
        private final Path mask = new Path();

        MaskedIcon(Drawable original, int backdrop) {
            this.original = original.mutate();
            this.backdrop = backdrop;
        }

        @Override
        protected void onBoundsChange(Rect bounds) {
            mask.rewind();
            float radius = bounds.width() * 0.25f;
            mask.addRoundRect(bounds.left, bounds.top, bounds.right, bounds.bottom,
                    radius, radius, Path.Direction.CW);
            if (original instanceof AdaptiveIconDrawable adaptive) {
                if (adaptive.getBackground() != null) adaptive.getBackground().setBounds(bounds);
                int expansion = Math.round(bounds.width() * AdaptiveIconDrawable.getExtraInsetFraction());
                if (adaptive.getForeground() != null) {
                    adaptive.getForeground().setBounds(bounds.left - expansion, bounds.top - expansion,
                            bounds.right + expansion, bounds.bottom + expansion);
                }
            } else {
                int inset = bounds.width() / 10;
                original.setBounds(bounds.left + inset, bounds.top + inset,
                        bounds.right - inset, bounds.bottom - inset);
            }
        }

        @Override
        public void draw(Canvas canvas) {
            int save = canvas.save();
            canvas.clipPath(mask);
            if (original instanceof AdaptiveIconDrawable adaptive) {
                if (adaptive.getBackground() != null) adaptive.getBackground().draw(canvas);
                if (adaptive.getForeground() != null) adaptive.getForeground().draw(canvas);
            } else {
                canvas.drawColor(backdrop);
                original.draw(canvas);
            }
            canvas.restoreToCount(save);
        }

        @Override public void setAlpha(int alpha) { original.setAlpha(alpha); }
        @Override public void setColorFilter(ColorFilter filter) { original.setColorFilter(filter); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }
}
