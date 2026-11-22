package org.wasmandroid.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.util.AttributeSet;
import android.view.View;
import android.widget.GridView;

/** Defers row binding until the weighted layout assigns a nonzero viewport. */
public class PreferenceGridView extends GridView {
    public PreferenceGridView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        if (MeasureSpec.getMode(heightSpec) != MeasureSpec.UNSPECIFIED
                && MeasureSpec.getSize(heightSpec) == 0) {
            setMeasuredDimension(MeasureSpec.getSize(widthSpec), 0);
        } else {
            super.onMeasure(widthSpec, heightSpec);
        }
    }

    @Override protected boolean drawChild(Canvas canvas, View child, long drawingTime) {
        final float top = child.getY();
        final int scroll = getScrollY();
        if (top >= scroll + getHeight() || top + child.getHeight() <= scroll) return false;
        return super.drawChild(canvas, child, drawingTime);
    }
}
