package android.app;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import java.util.LinkedHashMap;

public final class WallpaperManager {
    public static final int FLAG_SYSTEM = 1, FLAG_LOCK = 2;
    public interface OnColorsChangedListener {
        void onColorsChanged(WallpaperColors colors, int which);
    }
    private final WallpaperColors colors = new WallpaperColors(WallpaperColors.HINT_SUPPORTS_DARK_TEXT);
    private final LinkedHashMap<OnColorsChangedListener, Handler> listeners = new LinkedHashMap<>();
    private int desiredWidth, desiredHeight;

    public static WallpaperManager getInstance(Context context) {
        return context.getSystemService(WallpaperManager.class);
    }

    public WallpaperColors getWallpaperColors(int which) {
        if (which != FLAG_SYSTEM && which != FLAG_LOCK) throw new IllegalArgumentException();
        return which == FLAG_SYSTEM ? colors : null;
    }
    // The host exposes a static wallpaper and has no live-wallpaper engine.
    public WallpaperInfo getWallpaperInfo() { return null; }
    public void setWallpaperOffsetSteps(float xStep, float yStep) {}
    public void setWallpaperOffsets(IBinder windowToken, float x, float y) {}
    public void sendWallpaperCommand(IBinder windowToken, String action, int x, int y, int z, Bundle extras) {}
    public int getDesiredMinimumWidth() { return desiredWidth; }
    public int getDesiredMinimumHeight() { return desiredHeight; }
    public void suggestDesiredDimensions(int minimumWidth, int minimumHeight) {
        desiredWidth = minimumWidth;
        desiredHeight = minimumHeight;
    }
    public void addOnColorsChangedListener(OnColorsChangedListener listener, Handler handler) {
        listeners.put(java.util.Objects.requireNonNull(listener), handler);
    }
    public void removeOnColorsChangedListener(OnColorsChangedListener listener) {
        listeners.remove(listener);
    }
}
