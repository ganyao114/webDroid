package android.view;

import android.graphics.Rect;

public interface ViewParent {
    void requestLayout();
    boolean isLayoutRequested();
    ViewParent getParent();
    void requestDisallowInterceptTouchEvent(boolean disallowIntercept);
    default boolean onStartNestedScroll(View child, View target, int axes) { return false; }
    default boolean requestChildRectangleOnScreen(View child, Rect rect, boolean immediate) { return false; }
    void requestChildFocus(View child, View focused);
    void clearChildFocus(View child);
    void onDescendantInvalidated(View child, View target);
}
