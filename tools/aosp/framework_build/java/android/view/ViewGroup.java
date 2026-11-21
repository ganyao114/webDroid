package android.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.util.AttributeSet;
import java.util.ArrayList;

public abstract class ViewGroup extends View implements ViewParent {
    private android.animation.LayoutTransition layoutTransition;
    public android.animation.LayoutTransition getLayoutTransition() { return layoutTransition; }
    public void setLayoutTransition(android.animation.LayoutTransition value) { layoutTransition = value; }
    private final ArrayList<View> children = new ArrayList<>();
    private View touchTarget, focusedChild;
    private boolean disallowIntercept, clipChildren = true, clipToPadding = true;

    public interface OnHierarchyChangeListener {
        void onChildViewAdded(View parent, View child);
        void onChildViewRemoved(View parent, View child);
    }
    private OnHierarchyChangeListener hierarchyListener;

    public static class LayoutParams {
        public static final int MATCH_PARENT = -1, FILL_PARENT = -1, WRAP_CONTENT = -2;
        public int width, height;
        public LayoutParams(int width, int height) { this.width = width; this.height = height; }
        public LayoutParams(LayoutParams params) { this(params.width, params.height); }
        public LayoutParams(Context context, AttributeSet attrs) {
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.layout_width, android.R.attr.layout_height})) {
                width = a.getLayoutDimension(0, "layout_width");
                height = a.getLayoutDimension(1, "layout_height");
            }
        }
    }

    public static class MarginLayoutParams extends LayoutParams {
        private static final int UNSET = Integer.MIN_VALUE;
        private static final int[] MARGINS = {
            android.R.attr.layout_margin, android.R.attr.layout_marginLeft, android.R.attr.layout_marginTop,
            android.R.attr.layout_marginRight, android.R.attr.layout_marginBottom, android.R.attr.layout_marginStart,
            android.R.attr.layout_marginEnd, android.R.attr.layout_marginHorizontal, android.R.attr.layout_marginVertical,
        };
        public int leftMargin, topMargin, rightMargin, bottomMargin;
        private int start = UNSET, end = UNSET, direction;
        public MarginLayoutParams(int w, int h) { super(w, h); }
        public MarginLayoutParams(LayoutParams params) { super(params); if (params instanceof MarginLayoutParams) copy((MarginLayoutParams) params); }
        public MarginLayoutParams(MarginLayoutParams params) { super(params); copy(params); }
        public MarginLayoutParams(Context context, AttributeSet attrs) {
            super(context, attrs);
            try (TypedArray a = context.obtainStyledAttributes(attrs, MARGINS)) {
                int all = a.getDimensionPixelSize(0, UNSET);
                if (all != UNSET) setMargins(all, all, all, all);
                else {
                    int h = a.getDimensionPixelSize(7, UNSET), v = a.getDimensionPixelSize(8, UNSET);
                    setMargins(h == UNSET ? a.getDimensionPixelSize(1, 0) : h,
                            v == UNSET ? a.getDimensionPixelSize(2, 0) : v,
                            h == UNSET ? a.getDimensionPixelSize(3, 0) : h,
                            v == UNSET ? a.getDimensionPixelSize(4, 0) : v);
                    start = a.getDimensionPixelSize(5, UNSET);
                    end = a.getDimensionPixelSize(6, UNSET);
                }
            }
        }
        private void copy(MarginLayoutParams from) {
            setMargins(from.leftMargin, from.topMargin, from.rightMargin, from.bottomMargin);
            start = from.start; end = from.end; direction = from.direction;
        }
        public void setMargins(int l, int t, int r, int b) { leftMargin = l; topMargin = t; rightMargin = r; bottomMargin = b; }
        public void setMarginStart(int start) { this.start = start; }
        public void setMarginEnd(int end) { this.end = end; }
        public int getMarginStart() { return start != UNSET ? start : direction == LAYOUT_DIRECTION_RTL ? rightMargin : leftMargin; }
        public int getMarginEnd() { return end != UNSET ? end : direction == LAYOUT_DIRECTION_RTL ? leftMargin : rightMargin; }
        public boolean isMarginRelative() { return start != UNSET || end != UNSET; }
        public int getLayoutDirection() { return direction; }
        public void setLayoutDirection(int value) { direction = value; }
        public void resolveLayoutDirection(int value) { direction = value; }
        public int resolvedLeft(int direction) {
            int margin = direction == LAYOUT_DIRECTION_RTL ? end : start;
            return margin == UNSET ? leftMargin : margin;
        }
        public int resolvedRight(int direction) {
            int margin = direction == LAYOUT_DIRECTION_RTL ? start : end;
            return margin == UNSET ? rightMargin : margin;
        }
    }

    private ViewRootImpl attachment;
    public ViewGroup(Context context) { this(context, null); }
    public ViewGroup(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (attrs != null || defStyleAttr != 0 || defStyleRes != 0) {
            try (TypedArray a = context.obtainStyledAttributes(attrs,
                    new int[] {android.R.attr.clipChildren, android.R.attr.clipToPadding}, defStyleAttr, defStyleRes)) {
                clipChildren = a.getBoolean(0, true);
                clipToPadding = a.getBoolean(1, true);
            }
        }
    }

    @Override void attach(ViewRootImpl root) {
        attachment = root;
        super.attach(root);
        for (View child : children) child.attach(root);
    }

    @Override void detach() {
        for (View child : children) child.detach();
        attachment = null;
        super.detach();
    }

    public void setOnHierarchyChangeListener(OnHierarchyChangeListener listener) { hierarchyListener = listener; }
    public int getChildCount() { return children.size(); }
    public View getChildAt(int index) { return index < 0 || index >= children.size() ? null : children.get(index); }
    public int indexOfChild(View child) { return children.indexOf(child); }
    public void addView(View child) { addView(child, -1); }
    public void addView(View child, int index) { addView(child, index, child.getLayoutParams()); }
    public void addView(View child, int width, int height) { addView(child, -1, new LayoutParams(width, height)); }
    public void addView(View child, LayoutParams params) { addView(child, -1, params); }
    public void addView(View child, int index, LayoutParams params) {
        if (child.getParent() != null) throw new IllegalStateException("View already has a parent");
        if (params == null) params = generateDefaultLayoutParams();
        else if (!checkLayoutParams(params)) params = generateLayoutParams(params);
        child.setLayoutParams(params);
        if (index < 0) index = children.size();
        children.add(index, child);
        child.assignParent(this);
        if (attachment != null) child.attach(attachment);
        onViewAdded(child);
        if (hierarchyListener != null) hierarchyListener.onChildViewAdded(this, child);
        requestLayout();
        invalidate();
    }
    public void removeView(View child) { int index = indexOfChild(child); if (index >= 0) removeViewAt(index); }
    public void removeViewAt(int index) {
        View child = children.remove(index);
        if (touchTarget == child) touchTarget = null;
        if (attachment != null) child.detach();
        child.assignParent(null);
        onViewRemoved(child);
        if (hierarchyListener != null) hierarchyListener.onChildViewRemoved(this, child);
        requestLayout();
        invalidate();
    }
    public void removeViews(int start, int count) { for (int i = 0; i < count; ++i) removeViewAt(start); }
    public void removeAllViews() { removeViews(0, getChildCount()); }
    public void onViewAdded(View child) {}
    public void onViewRemoved(View child) {}
    public void bringChildToFront(View child) {
        int index = indexOfChild(child);
        if (index >= 0) { children.remove(index); children.add(child); requestLayout(); invalidate(); }
    }
    public void setClipChildren(boolean value) { clipChildren = value; invalidate(); }
    public boolean getClipChildren() { return clipChildren; }
    public void setClipToPadding(boolean value) { clipToPadding = value; invalidate(); }
    public boolean getClipToPadding() { return clipToPadding; }
    @Override protected View findViewTraversal(int id) {
        View self = super.findViewTraversal(id);
        if (self != null) return self;
        for (View child : children) {
            View match = child.findViewTraversal(id);
            if (match != null) return match;
        }
        return null;
    }

    public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    protected LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT); }
    protected LayoutParams generateLayoutParams(LayoutParams original) { return new LayoutParams(original); }
    protected boolean checkLayoutParams(LayoutParams params) { return params != null; }
    public static int getChildMeasureSpec(int spec, int padding, int dimension) {
        int available = Math.max(0, MeasureSpec.getSize(spec) - padding);
        int mode = MeasureSpec.getMode(spec);
        if (dimension >= 0) return MeasureSpec.makeMeasureSpec(dimension, MeasureSpec.EXACTLY);
        if (mode == MeasureSpec.UNSPECIFIED) return MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
        if (dimension == LayoutParams.MATCH_PARENT) return MeasureSpec.makeMeasureSpec(available, mode);
        return MeasureSpec.makeMeasureSpec(available, MeasureSpec.AT_MOST);
    }
    protected void measureChildren(int w, int h) {
        for (View child : children) if (child.getVisibility() != GONE) measureChild(child, w, h);
    }
    protected void measureChild(View child, int w, int h) {
        LayoutParams params = child.getLayoutParams();
        child.measure(getChildMeasureSpec(w, getPaddingLeft() + getPaddingRight(), params.width),
                getChildMeasureSpec(h, getPaddingTop() + getPaddingBottom(), params.height));
    }
    protected void measureChildWithMargins(View child, int w, int usedWidth, int h, int usedHeight) {
        MarginLayoutParams lp = (MarginLayoutParams) child.getLayoutParams();
        int direction = getLayoutDirection();
        child.measure(getChildMeasureSpec(w, getPaddingLeft() + getPaddingRight() + lp.resolvedLeft(direction)
                        + lp.resolvedRight(direction) + usedWidth, lp.width),
                getChildMeasureSpec(h, getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin + usedHeight,
                        lp.height));
    }

    @Override protected void dispatchDraw(Canvas canvas) {
        int state = canvas.getSaveCount();
        try {
            if (clipToPadding && (getPaddingLeft() | getPaddingTop() | getPaddingRight() | getPaddingBottom()) != 0) {
                canvas.save();
                canvas.clipRect(getScrollX() + getPaddingLeft(), getScrollY() + getPaddingTop(),
                        getScrollX() + getWidth() - getPaddingRight(), getScrollY() + getHeight() - getPaddingBottom());
            }
            long time = android.os.SystemClock.uptimeMillis();
            for (int i = 0; i < children.size(); i++) {
                View child = children.get(i);
                if (child.getVisibility() == VISIBLE) drawChild(canvas, child, time);
            }
        } finally {
            canvas.restoreToCount(state);
        }
    }
    @Override public WindowInsets dispatchApplyWindowInsets(WindowInsets insets) {
        WindowInsets result = super.dispatchApplyWindowInsets(insets);
        for (int i = 0; i < children.size(); i++) children.get(i).dispatchApplyWindowInsets(result);
        return result;
    }
    protected boolean drawChild(Canvas canvas, View child, long drawingTime) {
        if (child.getAlpha() <= 0) return false;
        int save = canvas.save();
        try {
            canvas.translate(child.getX(), child.getY());
            if (child.getAlpha() != 1f) canvas.multiplyAlpha(child.getAlpha());
            float rotation = child.getRotation(), sx = child.getScaleX(), sy = child.getScaleY();
            if (rotation != 0 || sx != 1f || sy != 1f) {
                float px = child.getPivotX(), py = child.getPivotY();
                if (rotation != 0) canvas.rotate(rotation, px, py);
                if (sx != 1f || sy != 1f) canvas.scale(sx, sy, px, py);
            }
            if (clipChildren) canvas.clipRect(0, 0, child.getWidth(), child.getHeight());
            child.draw(canvas);
        } finally {
            canvas.restoreToCount(save);
        }
        return true;
    }
    @Override public void onDescendantInvalidated(View child, View target) {
        if (getParent() != null) getParent().onDescendantInvalidated(this, target);
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            if (touchTarget != null) cancelTouchTarget(event);
            disallowIntercept = false;
        }
        boolean intercepted = !disallowIntercept && (action == MotionEvent.ACTION_DOWN || touchTarget != null)
                && onInterceptTouchEvent(event);
        if (intercepted && touchTarget != null) cancelTouchTarget(event);

        if (!intercepted && action == MotionEvent.ACTION_DOWN) {
            float x = event.getX() + getScrollX(), y = event.getY() + getScrollY();
            for (int i = children.size() - 1; i >= 0; i--) {
                View child = children.get(i);
                if (!hits(child, x, y)) continue;
                if (child.dispatchTouchEvent(localEvent(event, child))) {
                    touchTarget = child;
                    return true;
                }
            }
        }
        View target = touchTarget;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            touchTarget = null;
            disallowIntercept = false;
        }
        return target == null ? super.dispatchTouchEvent(event)
                : target.dispatchTouchEvent(localEvent(event, target));
    }
    private void cancelTouchTarget(MotionEvent event) {
        View child = touchTarget;
        touchTarget = null;
        MotionEvent cancel = localEvent(event, child);
        cancel.setAction(MotionEvent.ACTION_CANCEL);
        child.dispatchTouchEvent(cancel);
    }
    private MotionEvent localEvent(MotionEvent event, View child) {
        MotionEvent local = MotionEvent.obtain(event);
        local.offsetLocation(getScrollX() - child.getX(), getScrollY() - child.getY());
        return local;
    }
    private static boolean hits(View child, float x, float y) {
        return child.getVisibility() == VISIBLE && x >= child.getX() && x < child.getX() + child.getWidth()
                && y >= child.getY() && y < child.getY() + child.getHeight();
    }
    public boolean onInterceptTouchEvent(MotionEvent event) { return false; }
    @Override public void requestDisallowInterceptTouchEvent(boolean value) {
        if (disallowIntercept == value) return;
        disallowIntercept = value;
        if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(value);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        float x = event.getX() + getScrollX(), y = event.getY() + getScrollY();
        for (int i = children.size() - 1; i >= 0; --i) {
            View child = children.get(i);
            if (hits(child, x, y) && child.dispatchGenericMotionEvent(localEvent(event, child))) return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override public void requestChildFocus(View child, View focused) {
        if (isFocused()) setFocused(false, 0, null);
        View previous = focusedChild;
        if (previous != null && previous != child) previous.clearFocus();
        focusedChild = child;
        if (getParent() != null) getParent().requestChildFocus(this, focused);
    }
    @Override public void clearChildFocus(View child) {
        if (focusedChild == child) focusedChild = null;
        if (getParent() != null) getParent().clearChildFocus(this);
    }
    public View getFocusedChild() { return focusedChild; }
    @Override public View findFocus() { return focusedChild == null ? super.findFocus() : focusedChild.findFocus(); }
    @Override public boolean hasFocus() { return focusedChild != null || super.hasFocus(); }
    @Override public void clearFocus() { if (focusedChild != null) focusedChild.clearFocus(); else super.clearFocus(); }
    @Override public boolean requestFocus(int direction, Rect oldRect) {
        if (super.requestFocus(direction, oldRect)) {
            if (focusedChild != null) focusedChild.clearFocus();
            return true;
        }
        for (View child : children) if (child.requestFocus(direction, oldRect)) return true;
        return false;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        return (focusedChild != null && focusedChild.dispatchKeyEvent(event)) || super.dispatchKeyEvent(event);
    }
    @Override public void cancelLongPress() {
        super.cancelLongPress();
        for (int i = 0; i < children.size(); i++) children.get(i).cancelLongPress();
    }
}
