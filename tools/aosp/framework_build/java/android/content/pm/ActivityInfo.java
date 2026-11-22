package android.content.pm;

public class ActivityInfo extends ComponentInfo {
    public @interface Config {}
    public static final int CONFIG_MCC = 0x0001, CONFIG_MNC = 0x0002, CONFIG_LOCALE = 0x0004;
    public static final int CONFIG_TOUCHSCREEN = 0x0008, CONFIG_KEYBOARD = 0x0010, CONFIG_KEYBOARD_HIDDEN = 0x0020;
    public static final int CONFIG_NAVIGATION = 0x0040, CONFIG_ORIENTATION = 0x0080, CONFIG_SCREEN_LAYOUT = 0x0100;
    public static final int CONFIG_UI_MODE = 0x0200, CONFIG_SCREEN_SIZE = 0x0400, CONFIG_SMALLEST_SCREEN_SIZE = 0x0800;
    public static final int CONFIG_DENSITY = 0x1000, CONFIG_LAYOUT_DIRECTION = 0x2000, CONFIG_COLOR_MODE = 0x4000;
    public static final int CONFIG_FONT_SCALE = 0x40000000;
    public int theme, configChanges, launchMode, screenOrientation = -1;
    public String targetActivity, permission;
    public final int getThemeResource() { return theme != 0 ? theme : applicationInfo == null ? 0 : applicationInfo.theme; }
}
