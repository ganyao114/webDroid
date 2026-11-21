package android.view;

import android.graphics.Canvas;
import android.os.Handler;
import android.os.Looper;

/** Measures, lays out and draws the window in the host SurfaceFlinger transaction. */
public final class ViewRootImpl implements ViewParent {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final View decor;
    private final android.os.IBinder windowToken = new android.os.IBinder() {};
    private final Window window;
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
        this.window = window;
        decor.assignParent(this);
        decor.attach(this);
    }

    public View getView() { return decor; }
    public Handler getHandler() { return handler; }
    android.os.IBinder getWindowToken() { return windowToken; }
    public void detach() {
        decor.detach();
        decor.assignParent(null);
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
        if (insetsRequested && window != null) {
            insetsRequested = false;
            window.applyInsets();
        }
        if (requested) {
            requested = false;
            performMeasure();
            performLayout();
            drawRequested = true;
        }
        if (!drawRequested && !forceDraw) return false;
        drawRequested = false;
        nativeBeginDraw();
        performDraw(canvas);
        return true;
    }

    public void performMeasure() {
        decor.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
    }
    public void performLayout() { decor.layout(0, 0, width, height); }
    public void performDraw(Canvas canvas) {
        int saved = canvas.save();
        try {
            decor.draw(canvas);
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
    void requestApplyInsets() { insetsRequested = true; }
    @Override public boolean isLayoutRequested() { return requested; }
    @Override public ViewParent getParent() { return null; }
    @Override public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {}
    @Override public void requestChildFocus(View child, View focused) {}
    @Override public void clearChildFocus(View child) {}
    @Override public void onDescendantInvalidated(View child, View target) { drawRequested = true; }

    private static native void nativeBeginDraw();
}
