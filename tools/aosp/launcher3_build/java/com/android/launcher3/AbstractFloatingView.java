package com.android.launcher3;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.FrameLayout;
import com.android.launcher3.dragndrop.DragLayer;
import com.android.launcher3.views.ActivityContext;

/** Provides modal-view lookup to the upstream pager. */
public abstract class AbstractFloatingView extends FrameLayout {
    protected AbstractFloatingView(Context context, AttributeSet attrs) { super(context, attrs); }

    public static AbstractFloatingView getTopOpenView(ActivityContext activity) {
        DragLayer layer = activity.getDragLayer();
        if (layer == null) return null;
        for (int index = layer.getChildCount() - 1; index >= 0; index--) {
            if (layer.getChildAt(index) instanceof AbstractFloatingView floating
                    && floating.getVisibility() == VISIBLE) return floating;
        }
        return null;
    }
}
