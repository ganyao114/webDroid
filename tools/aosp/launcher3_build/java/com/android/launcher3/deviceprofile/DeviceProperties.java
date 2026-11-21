package com.android.launcher3.deviceprofile;

import android.content.res.Configuration;
import android.content.res.Resources;

public final class DeviceProperties {
    private final Resources resources;

    public DeviceProperties(Resources resources) { this.resources = resources; }
    public int getWidthPx() { return resources.getDisplayMetrics().widthPixels; }
    public int getHeightPx() { return resources.getDisplayMetrics().heightPixels; }
    public boolean isLargeScreen() { return resources.getConfiguration().smallestScreenWidthDp >= 600; }
    public boolean isPhone() { return !isLargeScreen(); }
    public boolean isLandscape() {
        return resources.getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
    }
    public boolean isTwoPanels() { return false; }
}
