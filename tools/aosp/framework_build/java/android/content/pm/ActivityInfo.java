package android.content.pm;

public class ActivityInfo extends ComponentInfo {
    public @interface Config {}
    public static final int CONFIG_LOCALE = 4, CONFIG_ORIENTATION = 0x80, CONFIG_SCREEN_SIZE = 0x400;
    public int theme, configChanges, launchMode, screenOrientation = -1;
    public String targetActivity, permission;
    public final int getThemeResource() { return theme != 0 ? theme : applicationInfo == null ? 0 : applicationInfo.theme; }
}
