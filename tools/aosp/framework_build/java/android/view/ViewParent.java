package android.view;

public interface ViewParent {
    void requestLayout();
    boolean isLayoutRequested();
    ViewParent getParent();
    void requestDisallowInterceptTouchEvent(boolean disallowIntercept);
    void requestChildFocus(View child, View focused);
    void clearChildFocus(View child);
    void onDescendantInvalidated(View child, View target);
}
