package android.app;

public final class WallpaperColors {
    public static final int HINT_SUPPORTS_DARK_TEXT = 1;
    private final int hints;
    WallpaperColors(int hints) { this.hints = hints; }
    public int getColorHints() { return hints; }
}
