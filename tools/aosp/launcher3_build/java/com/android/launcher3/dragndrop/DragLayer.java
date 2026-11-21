package com.android.launcher3.dragndrop;

import android.content.Context;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;
import com.android.launcher3.DeviceProfile;

public final class DragLayer extends FrameLayout {
    private final int slop;
    private final int openThreshold;
    private float downX;
    private float downY;
    private boolean tracking;
    private boolean swiping;
    private Runnable openDrawer;
    private boolean workspaceVisible = true;

    public DragLayer(Context context, AttributeSet attrs) {
        super(context, attrs);
        slop = ViewConfiguration.get(context).getScaledTouchSlop();
        openThreshold = DeviceProfile.pixels(getResources(), 44);
    }

    public void setOpenDrawer(Runnable action) { openDrawer = action; }
    public void setWorkspaceVisible(boolean visible) { workspaceVisible = visible; }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                tracking = workspaceVisible;
                swiping = false;
                downX = event.getX();
                downY = event.getY();
                break;
            case MotionEvent.ACTION_MOVE:
                if (tracking && isUpwardSwipe(event, slop)) {
                    swiping = true;
                    return true;
                }
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_UP:
                tracking = false;
                break;
            default:
                break;
        }
        return false;
    }

    private boolean isUpwardSwipe(MotionEvent event, int threshold) {
        float rise = downY - event.getY();
        return rise > threshold && rise > Math.abs(event.getX() - downX) * 1.3f;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!tracking) return false;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_UP:
                boolean open = isUpwardSwipe(event, openThreshold);
                tracking = false;
                if (open && openDrawer != null) openDrawer.run();
                else if (!swiping) performClick();
                break;
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_POINTER_DOWN:
                tracking = false;
                break;
            default:
                break;
        }
        return true;
    }

    @Override public boolean performClick() { return super.performClick(); }
}
