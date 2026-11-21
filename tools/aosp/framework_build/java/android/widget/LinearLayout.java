package android.widget;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

public class LinearLayout extends ViewGroup {
    public static final int HORIZONTAL = 0, VERTICAL = 1;
    public static class LayoutParams extends ViewGroup.MarginLayoutParams {
        public float weight;
        public int gravity = -1;
        public LayoutParams(Context context, AttributeSet attrs) {
            super(context, attrs);
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.layout_weight, android.R.attr.layout_gravity})) {
                weight = a.getFloat(0, 0);
                gravity = a.getInt(1, -1);
            }
        }
        public LayoutParams(int width, int height) { super(width, height); }
        public LayoutParams(int width, int height, float weight) { this(width, height); this.weight = weight; }
        public LayoutParams(ViewGroup.LayoutParams other) {
            super(other);
            if (other instanceof LayoutParams) {
                LayoutParams source = (LayoutParams) other;
                weight = source.weight;
                gravity = source.gravity;
            }
        }
        public LayoutParams(ViewGroup.MarginLayoutParams other) { this((ViewGroup.LayoutParams) other); }
        public LayoutParams(LayoutParams other) { this((ViewGroup.LayoutParams) other); }
    }

    private int orientation, gravity = Gravity.START | Gravity.TOP, length;
    private boolean baselineAligned = true;
    private int baselineChild = -1;
    private float weightSum = -1;
    public LinearLayout(Context context) { this(context, null); }
    public LinearLayout(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public LinearLayout(Context context, AttributeSet attrs, int defStyle) { this(context, attrs, defStyle, 0); }
    public LinearLayout(Context context, AttributeSet attrs, int defStyle, int defRes) {
        super(context, attrs, defStyle, defRes);
        if (attrs != null || defStyle != 0 || defRes != 0) {
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.orientation, android.R.attr.gravity, android.R.attr.weightSum,
                            android.R.attr.baselineAligned, android.R.attr.baselineAlignedChildIndex}, defStyle, defRes)) {
                orientation = a.getInt(0, HORIZONTAL);
                setGravity(a.getInt(1, gravity));
                weightSum = a.getFloat(2, -1);
                baselineAligned = a.getBoolean(3, true);
                baselineChild = a.getInt(4, -1);
            }
        }
    }

    public void setOrientation(int orientation) {
        if (this.orientation != orientation) { this.orientation = orientation; requestLayout(); }
    }
    public int getOrientation() { return orientation; }
    public void setGravity(int gravity) {
        if ((gravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) == 0) gravity |= Gravity.START;
        if ((gravity & Gravity.VERTICAL_GRAVITY_MASK) == 0) gravity |= Gravity.TOP;
        if (this.gravity != gravity) { this.gravity = gravity; requestLayout(); }
    }
    public int getGravity() { return gravity; }
    public void setHorizontalGravity(int value) { setGravity((gravity & ~Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK)
            | (value & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK)); }
    public void setVerticalGravity(int value) { setGravity((gravity & ~Gravity.VERTICAL_GRAVITY_MASK)
            | (value & Gravity.VERTICAL_GRAVITY_MASK)); }
    public void setWeightSum(float value) { weightSum = Math.max(0f, value); requestLayout(); }
    public float getWeightSum() { return weightSum; }
    public void setBaselineAligned(boolean enabled) { baselineAligned = enabled; requestLayout(); }
    public boolean isBaselineAligned() { return baselineAligned; }
    public void setBaselineAlignedChildIndex(int index) {
        if (index < 0 || index >= getChildCount()) throw new IllegalArgumentException("Baseline child index");
        baselineChild = index;
    }
    public int getBaselineAlignedChildIndex() { return baselineChild; }
    @Override public int getBaseline() {
        View child = getChildAt(baselineChild);
        return child == null || child.getBaseline() < 0 ? super.getBaseline() : child.getTop() + child.getBaseline();
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        boolean vertical = orientation == VERTICAL;
        int mainSpec = vertical ? heightSpec : widthSpec, crossSpec = vertical ? widthSpec : heightSpec;
        int mainPadding = vertical ? getPaddingTop() + getPaddingBottom() : getPaddingLeft() + getPaddingRight();
        int crossPadding = vertical ? getPaddingLeft() + getPaddingRight() : getPaddingTop() + getPaddingBottom();
        int dir = getLayoutDirection();
        int measuredLength = 0;
        float weight = 0;
        boolean exact = MeasureSpec.getMode(mainSpec) == MeasureSpec.EXACTLY;
        for (int i = 0; i < getChildCount(); ++i) {
            View child = getChildAt(i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams p = (LayoutParams) child.getLayoutParams();
            weight += Math.max(0, p.weight);
            int dimension = vertical ? p.height : p.width;
            int mainMargin = mainMargins(p, vertical, dir), crossMargin = crossMargins(p, vertical, dir);
            int main = dimension == 0 && p.weight > 0 && !exact ? LayoutParams.WRAP_CONTENT : dimension;
            int used = weight > 0 ? 0 : measuredLength;
            int childMain = getChildMeasureSpec(mainSpec, mainPadding + mainMargin + used, main);
            int childCross = getChildMeasureSpec(crossSpec, crossPadding + crossMargin, vertical ? p.width : p.height);
            measureChildAxes(child, vertical, childMain, childCross);
            measuredLength += mainSize(child, vertical) + mainMargin;
        }
        int minimum = vertical ? getSuggestedMinimumHeight() : getSuggestedMinimumWidth();
        int available = resolveSize(Math.max(minimum, measuredLength + mainPadding), mainSpec);
        int remaining = available - mainPadding - measuredLength;
        float totalWeight = weightSum > 0 ? weightSum : weight;
        int cross = 0, state = 0;
        int maxAscent = -1, maxDescent = -1;
        length = 0;
        for (int i = 0; i < getChildCount(); ++i) {
            View child = getChildAt(i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams p = (LayoutParams) child.getLayoutParams();
            int mainMargin = mainMargins(p, vertical, dir), crossMargin = crossMargins(p, vertical, dir);
            if (p.weight > 0 && totalWeight > 0 && remaining != 0) {
                int share = (int) (remaining * p.weight / totalWeight);
                remaining -= share;
                totalWeight -= p.weight;
                int size = Math.max(0, mainSize(child, vertical) + share);
                measureChildAxes(child, vertical, MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY),
                        getChildMeasureSpec(crossSpec, crossPadding + crossMargin, vertical ? p.width : p.height));
            }
            length += mainSize(child, vertical) + mainMargin;
            cross = Math.max(cross, crossSize(child, vertical) + crossMargin);
            int baseline = child.getBaseline();
            if (!vertical && baselineAligned && baseline >= 0 && p.height != LayoutParams.MATCH_PARENT) {
                maxAscent = Math.max(maxAscent, baseline + p.topMargin);
                maxDescent = Math.max(maxDescent, child.getMeasuredHeight() - baseline + p.bottomMargin);
            }
            state = combineMeasuredStates(state, child.getMeasuredState());
        }
        if (maxAscent >= 0) cross = Math.max(cross, maxAscent + maxDescent);
        int mainResult = resolveSizeAndState(Math.max(minimum, length + mainPadding), mainSpec,
                vertical ? state << MEASURED_HEIGHT_STATE_SHIFT : state);
        int crossResult = resolveSizeAndState(Math.max(cross + crossPadding,
                vertical ? getSuggestedMinimumWidth() : getSuggestedMinimumHeight()), crossSpec,
                vertical ? state : state << MEASURED_HEIGHT_STATE_SHIFT);
        setMeasuredDimension(vertical ? crossResult : mainResult, vertical ? mainResult : crossResult);
        if (MeasureSpec.getMode(crossSpec) == MeasureSpec.EXACTLY) return;
        for (int i = 0; i < getChildCount(); ++i) {
            View child = getChildAt(i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams p = (LayoutParams) child.getLayoutParams();
            if ((vertical ? p.width : p.height) != LayoutParams.MATCH_PARENT) continue;
            int size = Math.max(0, (crossResult & MEASURED_SIZE_MASK) - crossPadding - crossMargins(p, vertical, dir));
            measureChildAxes(child, vertical, MeasureSpec.makeMeasureSpec(mainSize(child, vertical), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY));
        }
    }
    private static void measureChildAxes(View child, boolean vertical, int main, int cross) {
        child.measure(vertical ? cross : main, vertical ? main : cross);
    }
    private static int mainSize(View child, boolean vertical) { return vertical ? child.getMeasuredHeight() : child.getMeasuredWidth(); }
    private static int crossSize(View child, boolean vertical) { return vertical ? child.getMeasuredWidth() : child.getMeasuredHeight(); }
    private static int mainMargins(LayoutParams p, boolean vertical, int dir) {
        return vertical ? p.topMargin + p.bottomMargin : p.resolvedLeft(dir) + p.resolvedRight(dir);
    }
    private static int crossMargins(LayoutParams p, boolean vertical, int dir) { return mainMargins(p, !vertical, dir); }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        if (orientation == VERTICAL) layoutVertical(r - l, b - t);
        else layoutHorizontal(r - l, b - t);
    }
    private void layoutVertical(int width, int height) {
        int left = getPaddingLeft(), right = width - getPaddingRight();
        int top = getPaddingTop(), bottom = height - getPaddingBottom();
        int y = coordinate(gravity & Gravity.VERTICAL_GRAVITY_MASK, top, bottom, length, 0, 0, false);
        int dir = getLayoutDirection();
        for (int i = 0; i < getChildCount(); ++i) {
            View child = getChildAt(i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams p = (LayoutParams) child.getLayoutParams();
            int childGravity = Gravity.getAbsoluteGravity(p.gravity == -1 ? gravity : p.gravity, dir);
            int w = child.getMeasuredWidth(), h = child.getMeasuredHeight();
            int x = coordinate(childGravity & Gravity.HORIZONTAL_GRAVITY_MASK, left, right, w,
                    p.resolvedLeft(dir), p.resolvedRight(dir), true);
            y += p.topMargin;
            child.layout(x, y, x + w, y + h);
            y += h + p.bottomMargin;
        }
    }
    private void layoutHorizontal(int width, int height) {
        int dir = getLayoutDirection(), left = getPaddingLeft(), right = width - getPaddingRight();
        int top = getPaddingTop(), bottom = height - getPaddingBottom();
        int absolute = Gravity.getAbsoluteGravity(gravity, dir);
        int x = coordinate(absolute & Gravity.HORIZONTAL_GRAVITY_MASK, left, right, length, 0, 0, true);
        int count = getChildCount();
        int maxBaseline = -1;
        if (baselineAligned) {
            for (int i = 0; i < count; ++i) {
                View child = getChildAt(i);
                LayoutParams p = (LayoutParams) child.getLayoutParams();
                int v = (p.gravity == -1 ? gravity : p.gravity) & Gravity.VERTICAL_GRAVITY_MASK;
                if (child.getVisibility() != GONE && v == Gravity.TOP && child.getBaseline() >= 0) {
                    maxBaseline = Math.max(maxBaseline, child.getBaseline() + p.topMargin);
                }
            }
        }
        for (int i = 0; i < count; ++i) {
            View child = getChildAt(dir == LAYOUT_DIRECTION_RTL ? count - i - 1 : i);
            if (child.getVisibility() == GONE) continue;
            LayoutParams p = (LayoutParams) child.getLayoutParams();
            int w = child.getMeasuredWidth(), h = child.getMeasuredHeight();
            int v = (p.gravity == -1 ? gravity : p.gravity) & Gravity.VERTICAL_GRAVITY_MASK;
            int y = coordinate(v, top, bottom, h, p.topMargin, p.bottomMargin, false);
            if (v == Gravity.TOP && maxBaseline >= 0 && child.getBaseline() >= 0) {
                y = top + maxBaseline - child.getBaseline();
            }
            x += p.resolvedLeft(dir);
            child.layout(x, y, x + w, y + h);
            x += w + p.resolvedRight(dir);
        }
    }
    private static int coordinate(int gravity, int start, int end, int size, int before, int after, boolean horizontal) {
        if (gravity == (horizontal ? Gravity.RIGHT : Gravity.BOTTOM)) return end - size - after;
        if (gravity == (horizontal ? Gravity.CENTER_HORIZONTAL : Gravity.CENTER_VERTICAL)) {
            return start + (end - start - size) / 2 + before - after;
        }
        return start + before;
    }

    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override protected LayoutParams generateDefaultLayoutParams() {
        return new LayoutParams(orientation == VERTICAL ? LayoutParams.MATCH_PARENT : LayoutParams.WRAP_CONTENT,
                LayoutParams.WRAP_CONTENT);
    }
    @Override protected LayoutParams generateLayoutParams(ViewGroup.LayoutParams params) { return new LayoutParams(params); }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams params) { return params instanceof LayoutParams; }
}
