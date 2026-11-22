package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import java.io.File;

public abstract class Context {
    private java.util.concurrent.Executor mainExecutor;
    public static final String LAYOUT_INFLATER_SERVICE = "layout_inflater";
    public static final String LAUNCHER_APPS_SERVICE = "launcherapps";
    public static final String INPUT_METHOD_SERVICE = "input_method";
    public static final String WINDOW_SERVICE = "window";
    public static final String DISPLAY_SERVICE = "display";
    public static final String USER_SERVICE = "user";
    public static final String APPWIDGET_SERVICE = "appwidget";
    public static final String WALLPAPER_SERVICE = "wallpaper";
    public static final String ACCESSIBILITY_SERVICE = "accessibility";
    public static final String SEARCH_SERVICE = "search";
    public static final int MODE_PRIVATE = 0;
    public static final int RECEIVER_EXPORTED = 2, RECEIVER_NOT_EXPORTED = 4;
    public static final int CONTEXT_INCLUDE_CODE = 1, CONTEXT_IGNORE_SECURITY = 2;
    public abstract AssetManager getAssets();
    public abstract Resources getResources();
    public abstract PackageManager getPackageManager();
    public abstract ContentResolver getContentResolver();
    public abstract String getPackageName();
    public String getOpPackageName() { return getPackageName(); }
    public int checkSelfPermission(String permission) {
        java.util.Objects.requireNonNull(permission);
        return PackageManager.PERMISSION_DENIED;
    }
    public abstract ApplicationInfo getApplicationInfo();
    public abstract Object getSystemService(String name);
    public abstract Resources.Theme getTheme();
    public abstract void setTheme(int id);
    public abstract void startActivity(Intent intent);
    public void startActivity(Intent intent, Bundle options) { startActivity(intent); }
    public Context getApplicationContext() { return this; }
    public ClassLoader getClassLoader() { return getClass().getClassLoader(); }
    public File getFilesDir() { return new File("/data/user/0/" + getPackageName() + "/files"); }
    public File getDatabasePath(String name) { return new File("/data/user/0/" + getPackageName() + "/databases", name); }
    public boolean deleteDatabase(String name) {
        return android.database.sqlite.SQLiteDatabase.deleteDatabase(getDatabasePath(name));
    }
    public void registerComponentCallbacks(ComponentCallbacks callback) {
        if (getApplicationContext() instanceof android.app.Application app) app.registerComponentCallbacks(callback);
    }
    public void unregisterComponentCallbacks(ComponentCallbacks callback) {
        if (getApplicationContext() instanceof android.app.Application app) app.unregisterComponentCallbacks(callback);
    }
    public SharedPreferences getSharedPreferences(String name, int mode) {
        throw new UnsupportedOperationException("This context has no shared preferences");
    }
    public Context createDeviceProtectedStorageContext() { return this; }
    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
        return registerReceiver(receiver, filter, null, null, 0);
    }
    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) {
        return registerReceiver(receiver, filter, null, null, flags);
    }
    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission, Handler handler) {
        return registerReceiver(receiver, filter, permission, handler, 0);
    }
    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                   Handler handler, int flags) {
        throw new UnsupportedOperationException("This context cannot register a receiver");
    }
    public void unregisterReceiver(BroadcastReceiver receiver) {
        throw new IllegalArgumentException("Receiver is not registered");
    }
    public void sendBroadcast(Intent intent) {
        throw new UnsupportedOperationException("This context cannot send a broadcast");
    }
    public Looper getMainLooper() { return Looper.getMainLooper(); }
    public java.util.concurrent.Executor getMainExecutor() {
        if (mainExecutor == null) mainExecutor = new android.os.HandlerExecutor(new Handler(getMainLooper()));
        return mainExecutor;
    }
    public android.view.Display getDisplay() {
        android.hardware.display.DisplayManager manager = getSystemService(android.hardware.display.DisplayManager.class);
        if (manager == null) throw new UnsupportedOperationException("No display associated with this context");
        return manager.getDisplay(android.view.Display.DEFAULT_DISPLAY);
    }
    public Context createWindowContext(android.view.Display display, int type, Bundle options) {
        if (display != getDisplay()) throw new IllegalArgumentException("Unknown display");
        return this;
    }
    public Context createWindowContext(int type, Bundle options) { return createWindowContext(getDisplay(), type, options); }
    public Context createDisplayContext(android.view.Display display) {
        if (display != getDisplay()) throw new IllegalArgumentException("Unknown display");
        return this;
    }
    public Context createConfigurationContext(android.content.res.Configuration override) {
        throw new UnsupportedOperationException("This context has no resources to reconfigure");
    }
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
        if (type == android.hardware.display.DisplayManager.class) return DISPLAY_SERVICE;
        if (type == android.os.UserManager.class) return USER_SERVICE;
        if (type == android.appwidget.AppWidgetManager.class) return APPWIDGET_SERVICE;
        if (type == android.app.WallpaperManager.class) return WALLPAPER_SERVICE;
        if (type == android.view.accessibility.AccessibilityManager.class) return ACCESSIBILITY_SERVICE;
        if (type == android.app.SearchManager.class) return SEARCH_SERVICE;
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
