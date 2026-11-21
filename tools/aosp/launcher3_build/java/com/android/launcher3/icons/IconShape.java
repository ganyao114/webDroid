package com.android.launcher3.icons;

import android.graphics.Path;
import android.graphics.Rect;

@FunctionalInterface
public interface IconShape {
    void addToPath(Path path, Rect bounds);
}
