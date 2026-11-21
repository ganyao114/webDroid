package android.widget;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

public class FrameLayout extends ViewGroup {
    public static class LayoutParams extends ViewGroup.MarginLayoutParams {
        public int gravity = -1;
        public LayoutParams(Context context, AttributeSet attrs) {
            super(context, attrs);
            try (TypedArray a = context.obtainStyledAttributes(attrs, new int[] {android.R.attr.layout_gravity})) {
                gravity = a.getInt(0, -1);
            }
        }
        public LayoutParams(int width, int height) { super(width, height); }
        public LayoutParams(int width, int height, int gravity) { this(width, height); this.gravity = gravity; }
        public LayoutParams(ViewGroup.LayoutParams params) {
            super(params);
            if (params instanceof LayoutParams) gravity = ((LayoutParams) params).gravity;
        }
        public LayoutParams(ViewGroup.MarginLayoutParams params) { this((ViewGroup.LayoutParams) params); }
        public LayoutParams(LayoutParams params) { this((ViewGroup.LayoutParams) params); }
    }

    private boolean measureAllChildren;
    public FrameLayout(Context context) { this(context, null); }
    public FrameLayout(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public FrameLayout(Context context, AttributeSet attrs, int defStyle) { this(context, attrs, defStyle, 0); }
    public FrameLayout(Context context, AttributeSet attrs, int defStyle, int defRes) {
        super(context, attrs, defStyle, defRes);
        if (attrs != null || defStyle != 0 || defRes != 0) {
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.measureAllChildren}, defStyle, defRes)) {
                measureAllChildren = a.getBoolean(0, false);
            }
        }
    }
    public void setMeasureAllChildren(boolean value) { measureAllChildren = value; requestLayout(); }
    public boolean getMeasureAllChildren() { return measureAllChildren; }
    @Deprecated public boolean getConsiderGoneChildrenWhenMeasuring() { return measureAllChildren; }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int maxWidth = 0, maxHeight = 0, state = 0;
        int direction = getLayoutDirection();
        boolean exactWidth = MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY;
        boolean exactHeight = MeasureSpec.getMode(heightSpec) == MeasureSpec.EXACTLY;
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            if (!measureAllChildren && child.getVisibility() == GONE) continue;
            LayoutParams params = (LayoutParams) child.getLayoutParams();
            measureChildWithMargins(child, widthSpec, 0, heightSpec, 0);
            maxWidth = Math.max(maxWidth, child.getMeasuredWidth() + params.resolvedLeft(direction) + params.resolvedRight(direction));
            maxHeight = Math.max(maxHeight, child.getMeasuredHeight() + params.topMargin + params.bottomMargin);
            state = combineMeasuredStates(state, child.getMeasuredState());
        }
        int pw = getPaddingLeft() + getPaddingRight(), ph = getPaddingTop() + getPaddingBottom();
        setMeasuredDimension(resolveSizeAndState(Math.max(getSuggestedMinimumWidth(), maxWidth + pw), widthSpec, state),
                resolveSizeAndState(Math.max(getSuggestedMinimumHeight(), maxHeight + ph), heightSpec,
                        state << MEASURED_HEIGHT_STATE_SHIFT));
        if (exactWidth && exactHeight) return;
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            if (!measureAllChildren && child.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) child.getLayoutParams();
            boolean fillWidth = lp.width == LayoutParams.MATCH_PARENT;
            boolean fillHeight = lp.height == LayoutParams.MATCH_PARENT;
            if (fillWidth || fillHeight) {
                int w = pw + lp.resolvedLeft(direction) + lp.resolvedRight(direction);
                int h = ph + lp.topMargin + lp.bottomMargin;
                child.measure(fillWidth ? MeasureSpec.makeMeasureSpec(Math.max(0, getMeasuredWidth() - w), MeasureSpec.EXACTLY)
                                : getChildMeasureSpec(widthSpec, w, lp.width),
                        fillHeight ? MeasureSpec.makeMeasureSpec(Math.max(0, getMeasuredHeight() - h), MeasureSpec.EXACTLY)
                                : getChildMeasureSpec(heightSpec, h, lp.height));
            }
        }
    }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int left = getPaddingLeft(), top = getPaddingTop();
        int right = r - l - getPaddingRight(), bottom = b - t - getPaddingBottom();
        int direction = getLayoutDirection();
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) child.getLayoutParams();
            int width = child.getMeasuredWidth(), height = child.getMeasuredHeight();
            int gravity = lp.gravity == -1 ? Gravity.START | Gravity.TOP : lp.gravity;
            int a = lp.resolvedLeft(direction), c = lp.resolvedRight(direction);
            int x, y;
            switch (Gravity.getAbsoluteGravity(gravity, direction) & Gravity.HORIZONTAL_GRAVITY_MASK) {
                case Gravity.CENTER_HORIZONTAL: x = left + (right - left - width) / 2 + a - c; break;
                case Gravity.RIGHT: x = right - width - c; break;
                default: x = left + a; break;
            }
            switch (gravity & Gravity.VERTICAL_GRAVITY_MASK) {
                case Gravity.CENTER_VERTICAL: y = top + (bottom - top - height) / 2 + lp.topMargin - lp.bottomMargin; break;
                case Gravity.BOTTOM: y = bottom - height - lp.bottomMargin; break;
                default: y = top + lp.topMargin; break;
            }
            child.layout(x, y, x + width, y + height);
        }
    }

    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override protected LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT); }
    @Override protected LayoutParams generateLayoutParams(ViewGroup.LayoutParams params) { return new LayoutParams(params); }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams params) { return params instanceof LayoutParams; }
}
