package com.android.launcher3.views;

import android.content.Context;
import android.os.Bundle;
import android.util.AttributeSet;
import android.view.View;
import android.view.accessibility.AccessibilityNodeInfo;

public final class AccessibilityActionsView extends View {
    private Runnable openDrawer;

    public AccessibilityActionsView(Context context, AttributeSet attrs) { super(context, attrs); }

    public void setOpenDrawer(Runnable openDrawer) { this.openDrawer = openDrawer; }

    @Override
    public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
    }

    @Override
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        if (action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD && openDrawer != null) {
            openDrawer.run();
            return true;
        }
        return super.performAccessibilityAction(action, arguments);
    }
}
