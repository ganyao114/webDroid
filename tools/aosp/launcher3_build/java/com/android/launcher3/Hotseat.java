package com.android.launcher3;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.util.AttributeSet;

public final class Hotseat extends CellLayout {
    public Hotseat(Context context, AttributeSet attrs) { super(context, attrs); }

    public void configure(DeviceProfile profile) {
        setGrid(profile.hotseatIcons, 1);
        int tint = getContext().getColor(R.color.materialColorSurfaceContainerHigh);
        int[] glass = {withAlpha(tint, 208), withAlpha(tint, 160)};
        GradientDrawable backdrop = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, glass);
        backdrop.setCornerRadius(DeviceProfile.pixels(getResources(), 28));
        backdrop.setStroke(DeviceProfile.pixels(getResources(), 1), withAlpha(Color.WHITE, 70));
        setBackground(backdrop);
        setPadding(DeviceProfile.pixels(getResources(), 6), 0,
                DeviceProfile.pixels(getResources(), 6), 0);
    }

    private static int withAlpha(int color, int alpha) { return (color & 0x00ffffff) | (alpha << 24); }

    // This offline dock contains pinned icons. Search belongs to the drawer.
    public android.view.View getQsb() { return this; }
}
