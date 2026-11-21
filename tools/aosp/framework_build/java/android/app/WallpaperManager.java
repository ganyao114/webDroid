package android.app;

import android.content.Context;

public final class WallpaperManager {
    public static final int FLAG_SYSTEM = 1, FLAG_LOCK = 2;
    private final WallpaperColors colors = new WallpaperColors(WallpaperColors.HINT_SUPPORTS_DARK_TEXT);

    public static WallpaperManager getInstance(Context context) {
        return context.getSystemService(WallpaperManager.class);
    }

    public WallpaperColors getWallpaperColors(int which) {
        if (which != FLAG_SYSTEM && which != FLAG_LOCK) throw new IllegalArgumentException();
        return which == FLAG_SYSTEM ? colors : null;
    }
}
