package android.view;

public interface WindowManager extends ViewManager {
    WindowMetrics getCurrentWindowMetrics();
    WindowMetrics getMaximumWindowMetrics();

    final class LayoutParams extends ViewGroup.LayoutParams {
        public static final int FLAG_SHOW_WALLPAPER = 0x00100000;
        public static final int FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS = 0x80000000;
        public static final int SOFT_INPUT_ADJUST_RESIZE = 0x10;
        public int flags, softInputMode;
        public LayoutParams() { super(MATCH_PARENT, MATCH_PARENT); }
    }
}
