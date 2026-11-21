package com.android.launcher3;

import android.content.Context;
import android.graphics.Insets;
import android.util.AttributeSet;
import android.view.WindowInsets;
import android.widget.FrameLayout;

public final class LauncherRootView extends FrameLayout {
    public LauncherRootView(Context context, AttributeSet attrs) { super(context, attrs); }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        setPadding(bars.left, bars.top, bars.right,
                Math.max(bars.bottom, insets.getInsets(WindowInsets.Type.ime()).bottom));
        return insets;
    }
}
