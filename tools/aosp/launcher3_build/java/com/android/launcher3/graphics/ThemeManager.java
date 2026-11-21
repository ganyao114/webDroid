package com.android.launcher3.graphics;

import android.graphics.Path;
import com.android.launcher3.icons.IconShape;
import com.android.launcher3.util.ContextProvider;

public final class ThemeManager {
    private static final ThemeManager SDK = new ThemeManager();
    public static final ContextProvider<ThemeManager> INSTANCE = context -> SDK;

    public IconShape getFileShapeData() {
        return (path, bounds) -> {
            float radius = bounds.width() * 0.24f;
            path.addRoundRect(bounds.left, bounds.top, bounds.right, bounds.bottom,
                    radius, radius, Path.Direction.CW);
        };
    }

    public IconShapeRef getIconShapeData() { return new IconShapeRef(getFileShapeData()); }
    public boolean isIconThemeEnabled() { return false; }

    public record IconShapeRef(IconShape value) {
        public IconShape getValue() { return value; }
    }
}
