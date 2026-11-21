package com.android.launcher3.views;

public record ScrimColors(int backgroundColor, int foregroundColor) {
    public int getBackgroundColor() { return backgroundColor; }
}
