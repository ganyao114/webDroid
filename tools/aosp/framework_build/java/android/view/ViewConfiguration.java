package android.view;

import android.content.Context;
import org.wasmandroid.ui.NativeMath;

public class ViewConfiguration {
    private final float density;
    public ViewConfiguration() { density = 1; }
    private ViewConfiguration(Context context) { density = context.getResources().getDisplayMetrics().density; }
    public static ViewConfiguration get(Context context) { return new ViewConfiguration(context); }
    public static int getTapTimeout() { return 100; }
    public static int getLongPressTimeout() { return 500; }
    public static int getDoubleTapTimeout() { return 300; }
    public static int getScrollDefaultDelay() { return 300; }
    public static float getScrollFriction() { return 0.015f; }
    public float getScrollFrictionAmount() { return getScrollFriction(); }
    public int getScaledTouchSlop() { return scaled(8); }
    public int getScaledPagingTouchSlop() { return scaled(16); }
    public int getScaledMinimumFlingVelocity() { return scaled(50); }
    public int getScaledMaximumFlingVelocity() { return scaled(8000); }
    public int getScaledOverflingDistance() { return scaled(6); }
    public int getScaledOverscrollDistance() { return 0; }
    public float getScaledVerticalScrollFactor() { return 64 * density; }
    public float getScaledHorizontalScrollFactor() { return 64 * density; }
    private int scaled(int dp) { return NativeMath.round(dp * density); }
}
