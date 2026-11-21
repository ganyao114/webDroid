package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Looper;
import android.util.AttributeSet;

public abstract class Context {
    public static final String LAYOUT_INFLATER_SERVICE = "layout_inflater";
    public static final String LAUNCHER_APPS_SERVICE = "launcherapps";
    public static final String INPUT_METHOD_SERVICE = "input_method";
    public static final String WINDOW_SERVICE = "window";
    public static final String WALLPAPER_SERVICE = "wallpaper";
    public static final String ACCESSIBILITY_SERVICE = "accessibility";
    public static final int CONTEXT_INCLUDE_CODE = 1, CONTEXT_IGNORE_SECURITY = 2;
    public abstract AssetManager getAssets();
    public abstract Resources getResources();
    public abstract PackageManager getPackageManager();
    public abstract ContentResolver getContentResolver();
    public abstract String getPackageName();
    public String getOpPackageName() { return getPackageName(); }
    public abstract ApplicationInfo getApplicationInfo();
    public abstract Object getSystemService(String name);
    public abstract Resources.Theme getTheme();
    public abstract void setTheme(int id);
    public abstract void startActivity(Intent intent);
    public void startActivity(Intent intent, Bundle options) { startActivity(intent); }
    public Context getApplicationContext() { return this; }
    public Looper getMainLooper() { return Looper.getMainLooper(); }
    public final CharSequence getText(int id) { return getResources().getText(id); }
    public final String getString(int id) { return getResources().getString(id); }
    public final String getString(int id, Object... args) { return getResources().getString(id, args); }
    public final int getColor(int id) { return getResources().getColor(id, getTheme()); }
    public final ColorStateList getColorStateList(int id) { return getResources().getColorStateList(id, getTheme()); }
    public final Drawable getDrawable(int id) { return getResources().getDrawable(id, getTheme()); }

    @SuppressWarnings("unchecked")
    public final <T> T getSystemService(Class<T> type) {
        String name = getSystemServiceName(type);
        if (name == null) return null;
        Object result = getSystemService(name);
        return type.isInstance(result) ? (T) result : null;
    }
    public String getSystemServiceName(Class<?> type) {
        if (type == android.view.LayoutInflater.class) return LAYOUT_INFLATER_SERVICE;
        if (type == android.content.pm.LauncherApps.class) return LAUNCHER_APPS_SERVICE;
        if (type == android.view.inputmethod.InputMethodManager.class) return INPUT_METHOD_SERVICE;
        if (type == android.view.WindowManager.class) return WINDOW_SERVICE;
        if (type == android.app.WallpaperManager.class) return WALLPAPER_SERVICE;
        if (type == android.view.accessibility.AccessibilityManager.class) return ACCESSIBILITY_SERVICE;
        return null;
    }
    public final TypedArray obtainStyledAttributes(int[] attrs) { return getTheme().obtainStyledAttributes(attrs); }
    public final TypedArray obtainStyledAttributes(int id, int[] attrs) { return getTheme().obtainStyledAttributes(id, attrs); }
    public final TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs) {
        return obtainStyledAttributes(set, attrs, 0, 0);
    }
    public final TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs, int defAttr, int defRes) {
        return getTheme().obtainStyledAttributes(set, attrs, defAttr, defRes);
    }
}
