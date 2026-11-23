package android.view;

import android.graphics.Canvas;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.inputmethod.InputMethodManager;

/** Measures, lays out and draws the window in the host SurfaceFlinger transaction. */
public final class ViewRootImpl implements ViewParent {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final View decor;
    private final android.os.IBinder windowToken = new android.os.IBinder() {};
    private final WindowId windowId = new WindowId(windowToken);
    private final EmbeddedWindow window;
    private final ViewTreeObserver treeObserver;
    private final View.AttachInfo attachInfo;
    final Thread mThread = Thread.currentThread();
    private View accessibilityFocusedHost;
    private boolean inLayout;
    private boolean requested = true;
    private boolean insetsRequested = true;
    private boolean drawRequested = true;
    private Canvas canvas;
    private int width, height, density;

    public ViewRootImpl(View decor) {
        this(decor, null);
    }

    public ViewRootImpl(View decor, Window window) {
        this.decor = decor;
        this.window = window instanceof EmbeddedWindow embedded ? embedded : null;
        treeObserver = new ViewTreeObserver(decor.getContext());
        attachInfo = new View.AttachInfo(this, decor.getContext(), handler, treeObserver, windowId);
        attachInfo.mRootView = decor;
        attachInfo.mAccessibilityWindowId = decor.getAccessibilityViewId();
        decor.assignParent(this);
        decor.attach(this);
    }

    public View getView() { return decor; }
    public Handler getHandler() { return handler; }
    public boolean hasWindowFocus() { return windowId.isFocused(); }
    public void setWindowFocus(boolean focused) {
        if (focused == attachInfo.mHasWindowFocus) return;
        attachInfo.mHasWindowFocus = focused;
        windowId.dispatchFocusChanged(focused);
        decor.dispatchWindowFocusChanged(focused);
        if (!focused) {
            View input = decor.findFocus();
            if (input != null) onViewFocusChanged(input, false);
            // The host pauses collection while rendering a frame. Deactivation can reclaim
            // discarded input and graphics peers between frames.
            System.gc();
        }
    }
    void onViewFocusChanged(View view, boolean focused) {
        var ime = view.getContext().getSystemService(InputMethodManager.class);
        if (ime != null) ime.onViewFocusChanged(view, focused);
    }
    ViewTreeObserver getTreeObserver() { return treeObserver; }
    View.AttachInfo getAttachInfo() { return attachInfo; }
    android.os.IBinder getWindowToken() { return windowToken; }
    WindowId getWindowId() { return windowId; }
    WindowInsets getRootWindowInsets() { return window == null ? null : window.makeInsets(); }
    WindowInsetsController getWindowInsetsController() { return window == null ? null : window.getInsetsController(); }
    void onSystemUiVisibilityChanged(View view) {
        if (window != null && view == decor) window.applyLegacySystemBarAppearance(view.getSystemUiVisibility());
        requestApplyInsets();
    }
    public void detach() {
        decor.detach();
        decor.assignParent(null);
        accessibilityFocusedHost = null;
        handler.removeCallbacksAndMessages(null);
    }

    public boolean doFrame(int width, int height, int density, boolean forceDraw) {
        if (this.width != width || this.height != height || this.density != density) {
            this.width = width; this.height = height; this.density = density;
            decor.getResources().updateDisplay(width, height, density);
            requested = true;
            insetsRequested = true;
            canvas = new Canvas(width, height, density);
        }
        if (attachInfo.mRecomputeGlobalAttributes) {
            attachInfo.mRecomputeGlobalAttributes = false;
            attachInfo.mKeepScreenOn = false;
            attachInfo.mSystemUiVisibility = 0;
            decor.dispatchCollectViewAttributes(attachInfo, View.VISIBLE);
        }
        if (insetsRequested && window != null) {
            insetsRequested = false;
            window.applyInsets();
        }
        if (requested) {
            performMeasure();
            // Measurement may request layout while fitting its children. This
            // pass handles those requests and keeps any made during layout.
            requested = false;
            performLayout();
            treeObserver.dispatchOnGlobalLayout();
            drawRequested = true;
        }
        if (!drawRequested && !forceDraw) return false;
        if (treeObserver.dispatchOnPreDraw()) return false;
        drawRequested = false;
        attachInfo.mDrawingTime = SystemClock.uptimeMillis();
        nativeBeginDraw();
        treeObserver.dispatchOnDraw();
        performDraw(canvas);
        return true;
    }

    public void performMeasure() {
        decor.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
    }
    public void performLayout() {
        inLayout = true;
        try {
            decor.layout(0, 0, width, height);
        } finally {
            inLayout = false;
        }
    }
    public void performDraw(Canvas canvas) {
        int saved = canvas.save();
        try {
            decor.drawRoot(canvas);
        } finally {
            canvas.restoreToCount(saved);
        }
    }
    public boolean dispatchTouchEvent(MotionEvent event) { return decor.dispatchTouchEvent(event); }
    public boolean dispatchKeyEvent(KeyEvent event) { return decor.dispatchKeyEvent(event); }
    public boolean dispatchGenericMotionEvent(MotionEvent event) { return decor.dispatchGenericMotionEvent(event); }
    public boolean dispatchImeText(CharSequence text, boolean composing) {
        View focused = decor.findFocus();
        return focused != null && focused.onImeText(text, composing);
    }
    @Override public void requestLayout() { requested = true; }
    boolean isInLayout() { return inLayout; }
    boolean requestLayoutDuringLayout(View view) {
        requestLayout();
        return true;
    }
    boolean ensureTouchMode(boolean inTouchMode) {
        if (attachInfo.mInTouchMode == inTouchMode) return false;
        attachInfo.mInTouchMode = inTouchMode;
        treeObserver.dispatchOnTouchModeChanged(inTouchMode);
        return true;
    }
    void getWindowVisibleDisplayFrame(Rect out) {
        if (width > 0 && height > 0) {
            out.set(0, 0, width, height);
        } else {
            var metrics = decor.getResources().getDisplayMetrics();
            out.set(0, 0, metrics.widthPixels, metrics.heightPixels);
        }
    }
    void requestApplyInsets() { insetsRequested = true; }
    @Override public boolean isLayoutRequested() { return requested; }
    @Override public ViewParent getParent() { return null; }
    @Override public int getLayoutDirection() { return decor.getLayoutDirection(); }
    @Override public void bringChildToFront(View child) {}
    @Override public void requestFitSystemWindows() { requestApplyInsets(); }
    @Override public void recomputeViewAttributes(View child) {
        attachInfo.mRecomputeGlobalAttributes = true;
        onSystemUiVisibilityChanged(child);
    }
    @Override public boolean getChildVisibleRect(View child, Rect bounds, Point offset) {
        if (child != decor) return false;
        if (!child.hasIdentityMatrix()) {
            RectF transformed = attachInfo.mTmpTransformRect;
            transformed.set(bounds);
            child.getMatrix().mapRect(transformed);
            transformed.roundOut(bounds);
            if (offset != null) {
                float[] point = attachInfo.mTmpTransformLocation;
                point[0] = offset.x;
                point[1] = offset.y;
                child.getMatrix().mapPoints(point);
                offset.set(Math.round(point[0]), Math.round(point[1]));
            }
        }
        return bounds.intersect(0, 0, width, height);
    }
    @Override public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {}
    @Override public void requestChildFocus(View child, View focused) {}
    @Override public void clearChildFocus(View child) {}
    @Override public void onDescendantInvalidated(View child, View target) { drawRequested = true; }
    @Override public View focusSearch(View focused, int direction) {
        return decor instanceof ViewGroup group
                ? FocusFinder.getInstance().findNextFocus(group, focused, direction) : null;
    }

    static boolean isViewDescendantOf(View child, View parent) {
        for (View current = child; current != null;) {
            if (current == parent) return true;
            current = current.getParent() instanceof View next ? next : null;
        }
        return false;
    }
    View getAccessibilityFocusedHost() { return accessibilityFocusedHost; }
    void setAccessibilityFocus(View host, AccessibilityNodeInfo node) {
        View previous = accessibilityFocusedHost;
        accessibilityFocusedHost = host;
        if (previous != null && previous != host) previous.clearAccessibilityFocusNoCallbacks(0);
    }

    void dispatchInvalidateDelayed(View view, long delay) {
        handler.postDelayed(view::invalidate, view, delay);
    }
    void dispatchInvalidateRectDelayed(View.AttachInfo.InvalidateInfo info, long delay) {
        View target = info.target;
        handler.postDelayed(() -> invalidateRect(info), target, delay);
    }
    void dispatchInvalidateOnAnimation(View view) {
        Choreographer.getInstance().postCallback(Choreographer.CALLBACK_ANIMATION, view::invalidate, view);
    }
    void dispatchInvalidateRectOnAnimation(View.AttachInfo.InvalidateInfo info) {
        Choreographer.getInstance().postCallback(
                Choreographer.CALLBACK_ANIMATION, () -> invalidateRect(info), info.target);
    }
    void cancelInvalidate(View view) {
        handler.removeCallbacksAndMessages(view);
        Choreographer.getInstance().removeCallbacks(Choreographer.CALLBACK_ANIMATION, null, view);
    }
    private static void invalidateRect(View.AttachInfo.InvalidateInfo info) {
        try {
            info.target.invalidate(info.left, info.top, info.right, info.bottom);
        } finally {
            info.recycle();
        }
    }

    private static native void nativeBeginDraw();
}
