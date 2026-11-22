package android.app;

import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.LauncherApps;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.LocaleList;
import android.os.UserHandle;
import android.os.UserManager;
import android.view.Choreographer;
import android.view.Display;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import com.android.internal.policy.PhoneLayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowMetrics;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

/** Entry points invoked by UiBridge on the interpreter thread. */
public final class UiRuntime {
    private static Activity current;
    private static AppContext context;
    private static final ArrayList<Pointer> pointers = new ArrayList<>();
    private static long downTime, keyDownTime;

    private UiRuntime() {}

    private static final class Pointer {
        final int id;
        float x, y;
        Pointer(int id, float x, float y) { this.id = id; this.x = x; this.y = y; }
    }
    private static final class AppContext extends Context {
        private static final android.util.ArrayMap<String, android.util.ArrayMap<String, SharedPreferences>>
                packagePreferences = new android.util.ArrayMap<>();
        final Resources resources;
        final ApplicationInfo info;
        Application application;
        private final DisplayManager displays;
        private final WindowManager windows;
        private final UserManager users = new UserManager();
        private final AppWidgetManager widgets = new AppWidgetManager();
        private final ArrayList<Resources> configuredResources = new ArrayList<>();
        private static final class ReceiverRegistration {
            final BroadcastReceiver receiver;
            final IntentFilter filter;
            final Handler handler;
            ReceiverRegistration(BroadcastReceiver receiver, IntentFilter filter, Handler handler) {
                this.receiver = receiver;
                this.filter = filter;
                this.handler = handler;
            }
        }
        private final ArrayList<ReceiverRegistration> receivers = new ArrayList<>();
        final PackageManager packages;
        final android.content.ContentResolver resolver = new android.content.ContentResolver();
        private LayoutInflater inflater;
        private LauncherApps launcher;
        private WallpaperManager wallpaper;
        private SearchManager search;
        private android.view.inputmethod.InputMethodManager inputMethod;
        private android.view.accessibility.AccessibilityManager accessibility;
        private Resources.Theme theme;

        AppContext(Resources resources, ApplicationInfo info) {
            this.resources = resources; this.info = info;
            packages = new PackageManager(this);
            displays = new DisplayManager(new Display(resources));
            windows = new WindowManager() {
                @Override public Display getDefaultDisplay() { return getDisplay(); }
                @Override public WindowMetrics getCurrentWindowMetrics() { return Window.displayMetrics(AppContext.this); }
                @Override public WindowMetrics getMaximumWindowMetrics() { return getCurrentWindowMetrics(); }
                @Override public void addView(View view, ViewGroup.LayoutParams params) {
                    throw new IllegalStateException("Window is required to add a view");
                }
                @Override public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
                    throw new IllegalStateException("Window is required to update a view");
                }
                @Override public void removeView(View view) {
                    throw new IllegalStateException("Window is required to remove a view");
                }
            };
        }
        @Override public AssetManager getAssets() { return resources.getAssets(); }
        @Override public Resources getResources() { return resources; }
        @Override public String getPackageName() { return info.packageName; }
        @Override public Context getApplicationContext() { return application == null ? this : application; }
        @Override public Context createConfigurationContext(Configuration override) {
            Resources configured = resources.withConfiguration(override);
            configuredResources.add(configured);
            return new ContextWrapper(this) {
                private Resources.Theme theme;
                @Override public Resources getResources() { return configured; }
                @Override public AssetManager getAssets() { return configured.getAssets(); }
                @Override public Resources.Theme getTheme() {
                    if (theme == null) {
                        theme = configured.newTheme();
                        theme.setTo(AppContext.this.getTheme());
                    }
                    return theme;
                }
                @Override public void setTheme(int id) { getTheme().applyStyle(id, true); }
            };
        }
        void closeResources() {
            receivers.clear();
            for (Resources configured : configuredResources) configured.close();
            configuredResources.clear();
            packages.closePackageResources();
            resources.close();
        }
        @Override public SharedPreferences getSharedPreferences(String name, int mode) {
            synchronized (packagePreferences) {
                return packagePreferences.computeIfAbsent(info.packageName, key -> new android.util.ArrayMap<>())
                        .computeIfAbsent(name, key -> new MemorySharedPreferences(info.targetSdkVersion >= 30));
            }
        }
        @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                                 Handler handler, int flags) {
            if (receiver != null && (permission == null || permission.isEmpty())) {
                receivers.add(new ReceiverRegistration(receiver, filter, handler == null ? new Handler(getMainLooper()) : handler));
            }
            return null;
        }
        @Override public void unregisterReceiver(BroadcastReceiver receiver) {
            boolean removed = false;
            for (int i = receivers.size() - 1; i >= 0; i--) {
                if (receivers.get(i).receiver == receiver) {
                    receivers.remove(i);
                    removed = true;
                }
            }
            if (!removed) super.unregisterReceiver(receiver);
        }
        @Override public void sendBroadcast(Intent intent) {
            if (intent.getPackage() != null && !intent.getPackage().equals(getPackageName())) return;
            var data = intent.getData();
            String scheme = data == null ? null : data.getScheme();
            for (ReceiverRegistration registration : receivers) {
                if (registration.filter.match(intent.getAction(), intent.getType(), scheme,
                        data, intent.getCategories(), "BroadcastReceiver") < 0) continue;
                registration.handler.post(() -> {
                    if (!receivers.contains(registration)) return;
                    BroadcastReceiver receiver = registration.receiver;
                    var result = new BroadcastReceiver.PendingResult(0, null, null,
                            BroadcastReceiver.PendingResult.TYPE_REGISTERED, false, false, null, 0, 0);
                    receiver.setPendingResult(result);
                    try {
                        receiver.onReceive(getApplicationContext(), intent);
                    } finally {
                        if (receiver.getPendingResult() == result) result.finish();
                        receiver.setPendingResult(null);
                    }
                });
            }
        }
        @Override public ApplicationInfo getApplicationInfo() { return info; }
        @Override public PackageManager getPackageManager() { return packages; }
        @Override public android.content.ContentResolver getContentResolver() { return resolver; }
        @Override public Resources.Theme getTheme() {
            if (theme == null) {
                theme = resources.newTheme();
                if (info.theme != 0) theme.applyStyle(info.theme, true);
            }
            return theme;
        }
        @Override public void setTheme(int id) { info.theme = id; theme = null; }
        @Override public void startActivity(Intent intent) { startActivity(intent, null); }
        @Override public void startActivity(Intent intent, Bundle options) { packages.launch(intent, UserHandle.SYSTEM, options); }
        @Override public Object getSystemService(String name) {
            if (DISPLAY_SERVICE.equals(name)) return displays;
            if (WINDOW_SERVICE.equals(name)) return windows;
            if (USER_SERVICE.equals(name)) return users;
            if (APPWIDGET_SERVICE.equals(name)) return widgets;
            if (SEARCH_SERVICE.equals(name)) {
                if (search == null) search = new SearchManager(this);
                return search;
            }
            if (LAYOUT_INFLATER_SERVICE.equals(name)) {
                if (inflater == null) inflater = new PhoneLayoutInflater(this);
                return inflater;
            }
            if (LAUNCHER_APPS_SERVICE.equals(name)) {
                if (launcher == null) launcher = new LauncherApps(this);
                return launcher;
            }
            if (INPUT_METHOD_SERVICE.equals(name)) {
                if (inputMethod == null) inputMethod = new android.view.inputmethod.InputMethodManager();
                return inputMethod;
            }
            if (WALLPAPER_SERVICE.equals(name)) {
                if (wallpaper == null) wallpaper = new WallpaperManager();
                return wallpaper;
            }
            if (ACCESSIBILITY_SERVICE.equals(name)) {
                if (accessibility == null) accessibility = new android.view.accessibility.AccessibilityManager();
                return accessibility;
            }
            return null;
        }
    }
    private static final class Manifest {
        final ApplicationInfo info = new ApplicationInfo();
        String application;
        String activity;
        int theme;
    }

    public static void launch(String apkPath, String requested, String locale, String frameworkApk,
                              int density, int width, int height)
            throws IOException, XmlPullParserException {
        destroy();
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        boolean withFramework = frameworkApk != null && !frameworkApk.isEmpty();
        String[] assetPaths = withFramework ? new String[] {frameworkApk, apkPath} : new String[] {apkPath};
        Resources resources = Resources.forApk(assetPaths, locale, density);
        try {
            LocaleList locales = resources.getConfiguration().getLocales();
            if (!locales.isEmpty()) LocaleList.setDefault(locales);
            if (width > 0 && height > 0) resources.updateDisplay(width, height, density);
            Manifest manifest = readManifest(resources, apkPath, requested, withFramework ? 1 : 0);
            AppContext app = new AppContext(resources, manifest.info);
            context = app;
            app.application = createApplication(app, manifest.application);
            app.application.onCreate();
            Activity activity = nativeCreateActivity(manifest.activity);
            ComponentName component = new ComponentName(manifest.info.packageName, manifest.activity);
            activity.attach(app, Intent.makeMainActivity(component), manifest.theme);
            current = activity;
            activity.performCreate(null);
            finishIfRequested();
        } catch (IOException | XmlPullParserException | RuntimeException | Error error) {
            try {
                if (context != null) destroy();
                else resources.close();
            } catch (RuntimeException | Error cleanup) {
                if (cleanup != error) error.addSuppressed(cleanup);
            }
            throw error;
        }
    }

    private static Application createApplication(AppContext context, String name) {
        Application application;
        try {
            application = name == null ? new Application()
                    : (Application) Class.forName(name).getDeclaredConstructor().newInstance();
        } catch (ReflectiveOperationException error) {
            throw new IllegalArgumentException("Cannot instantiate application " + name, error);
        }
        application.attach(context);
        return application;
    }

    static String currentProcessName() { return context == null ? null : context.getPackageName(); }

    private static Manifest readManifest(Resources res, String path, String requested, int baseIndex)
            throws IOException, XmlPullParserException {
        Manifest result = new Manifest();
        try (XmlResourceParser xml = res.getAssets().getApkAssets()[baseIndex].openXml("AndroidManifest.xml")) {
            XmlValues.toStartTag(xml);
            if (!"manifest".equals(xml.getName())) throw new XmlPullParserException("APK has no manifest");
            result.info.packageName = xml.getAttributeValue(null, "package");
            if (result.info.packageName == null) throw new XmlPullParserException("Manifest has no package name");
            result.info.sourceDir = result.info.publicSourceDir = path;
            String desired = requested == null || requested.length() == 0 ? null : fullName(result.info.packageName, requested);
            String fallback = null;
            int fallbackTheme = 0;
            for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
                if (event != XmlPullParser.START_TAG) continue;
                if (xml.getDepth() == 2 && "application".equals(xml.getName())) {
                    String application = xml.getAttributeValue(XmlValues.ANDROID, "name");
                    if (application != null) result.application = fullName(result.info.packageName, application);
                    result.info.icon = xml.getAttributeResourceValue(XmlValues.ANDROID, "icon", 0);
                    if (xml.getAttributeBooleanValue(XmlValues.ANDROID, "supportsRtl", false)) {
                        result.info.flags |= ApplicationInfo.FLAG_SUPPORTS_RTL;
                    }
                    result.info.theme = xml.getAttributeResourceValue(XmlValues.ANDROID, "theme", 0);
                    result.info.labelRes = xml.getAttributeResourceValue(XmlValues.ANDROID, "label", 0);
                    if (result.info.labelRes == 0) result.info.nonLocalizedLabel = xml.getAttributeValue(XmlValues.ANDROID, "label");
                } else if (xml.getDepth() == 2 && "uses-sdk".equals(xml.getName())) {
                    result.info.targetSdkVersion = xml.getAttributeIntValue(XmlValues.ANDROID, "targetSdkVersion", 37);
                } else if (xml.getDepth() == 3 && "activity".equals(xml.getName())) {
                    String name = xml.getAttributeValue(XmlValues.ANDROID, "name");
                    if (name == null) continue;
                    name = fullName(result.info.packageName, name);
                    int theme = xml.getAttributeResourceValue(XmlValues.ANDROID, "theme", 0);
                    if (fallback == null) { fallback = name; fallbackTheme = theme; }
                    if (desired != null && desired.equals(name)) {
                        result.activity = name; result.theme = theme;
                    } else if (desired == null && result.activity == null && isLauncherActivity(xml)) {
                        result.activity = name; result.theme = theme;
                    }
                }
            }
            if (result.activity == null && desired == null) {
                result.activity = fallback; result.theme = fallbackTheme;
            }
            if (result.activity == null) throw new IllegalArgumentException("Activity absent from manifest " + requested);
        }
        return result;
    }
    private static boolean isLauncherActivity(XmlResourceParser xml) throws IOException, XmlPullParserException {
        int depth = xml.getDepth(), filterDepth = 0;
        boolean action = false, category = false, launcher = false;
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG) {
                if (xml.getDepth() == depth) break;
                if (xml.getDepth() == filterDepth) {
                    launcher |= action && category;
                    filterDepth = 0;
                }
            } else if (event == XmlPullParser.START_TAG) {
                String tag = xml.getName();
                if ("intent-filter".equals(tag) && xml.getDepth() == depth + 1) {
                    filterDepth = xml.getDepth(); action = category = false;
                } else if (filterDepth != 0 && xml.getDepth() == filterDepth + 1) {
                    String name = xml.getAttributeValue(XmlValues.ANDROID, "name");
                    if ("action".equals(tag)) action |= Intent.ACTION_MAIN.equals(name);
                    if ("category".equals(tag)) category |= Intent.CATEGORY_LAUNCHER.equals(name) || Intent.CATEGORY_HOME.equals(name);
                }
            }
        }
        return launcher;
    }
    private static String fullName(String pkg, String name) {
        if (name.startsWith(".")) return pkg + name;
        return name.indexOf('.') < 0 ? pkg + "." + name : name;
    }

    public static Activity getActivity() { return current; }
    public static void sendHome() {
        if (current != null) current.onNewIntent(Intent.makeMainActivity(current.getComponentName())
                .addCategory(Intent.CATEGORY_HOME));
    }
    public static void sendBack() { if (current != null) current.performBackPressed(); }
    public static void sendAllApps() {
        if (current != null) current.onNewIntent(new Intent(Intent.ACTION_ALL_APPS)
                .setComponent(current.getComponentName()));
    }
    public static void packageChanged(String name, boolean replaced) {
        if (context != null && context.launcher != null) context.launcher.dispatchPackageChanged(name, replaced);
    }
    public static void pause() { if (current != null) current.performPause(); finishIfRequested(); }
    public static void stop() { if (current != null) current.performStop(); finishIfRequested(); }
    public static void resume() { if (current != null) current.performResume(); finishIfRequested(); }
    public static void destroy() {
        Activity old = current;
        AppContext app = context;
        current = null;
        context = null;
        pointers.clear();
        try {
            if (old != null) old.performDestroy();
        } finally {
            if (app != null) app.closeResources();
        }
    }
    private static void finishIfRequested() { if (current != null && current.isFinishing()) destroy(); }

    private static int frameWidth, frameHeight, frameDensity;
    private static boolean drew, forceDraw;
    private static final Runnable TRAVERSE = () -> {
        if (current != null && current.isResumed() && !current.isFinishing())
            drew = current.viewRoot().doFrame(frameWidth, frameHeight, frameDensity, forceDraw);
    };

    /** Return 0 after destruction, 1 while paused, 2 after drawing or 3 for a retained frame. */
    public static int doFrame(long nanos, int width, int height, int dpi, boolean force) {
        Looper looper = Looper.getMainLooper();
        if (looper == null) return 0;
        if (context != null) {
            var display = context.resources.getDisplayMetrics();
            if (width != display.widthPixels || height != display.heightPixels || dpi != display.densityDpi) {
                context.resources.updateDisplay(width, height, dpi);
                context.application.onConfigurationChanged(context.resources.getConfiguration());
                context.displays.dispatchDisplayChanged();
                current.onConfigurationChanged(context.resources.getConfiguration());
            }
        }
        Looper.dispatchAllDueMessages(nanos / 1_000_000);
        boolean draw = current != null && current.isResumed() && !current.isFinishing();
        frameWidth = width;
        frameHeight = height;
        frameDensity = dpi;
        forceDraw = force;
        drew = false;
        Choreographer.getInstance().doFrame(nanos, draw ? TRAVERSE : null);
        finishIfRequested();
        if (current == null) return 0;
        if (!current.isResumed()) return 1;
        return drew ? 2 : 3;
    }

    public static boolean dispatchMotion(long time, int action, int id, int meta, float x, float y, float dx, float dy) {
        if (current == null || !current.isResumed()) return false;
        if (action == MotionEvent.ACTION_SCROLL) {
            MotionEvent event = MotionEvent.fromHost(time, time, action, new int[] {id}, new float[] {x},
                    new float[] {y}, meta, dx, dy);
            boolean handled = current.dispatchGenericMotionEvent(event);
            finishIfRequested();
            return handled;
        }
        int index = 0;
        while (index < pointers.size() && pointers.get(index).id != id) index++;
        if (action == MotionEvent.ACTION_DOWN && index == pointers.size()) {
            if (pointers.isEmpty()) downTime = time;
            pointers.add(new Pointer(id, x, y));
            if (pointers.size() > 1) action = MotionEvent.ACTION_POINTER_DOWN | index << 8;
        } else if (index < pointers.size()) {
            Pointer p = pointers.get(index);
            p.x = x; p.y = y;
            if (action == MotionEvent.ACTION_UP && pointers.size() > 1) action = MotionEvent.ACTION_POINTER_UP | index << 8;
        } else if (action != MotionEvent.ACTION_CANCEL || pointers.isEmpty()) {
            return false;
        }
        int[] ids = new int[pointers.size()];
        float[] xs = new float[ids.length], ys = new float[ids.length];
        for (int i = 0; i < ids.length; ++i) {
            Pointer p = pointers.get(i);
            ids[i] = p.id; xs[i] = p.x; ys[i] = p.y;
        }
        MotionEvent event = MotionEvent.fromHost(downTime, time, action, ids, xs, ys, meta, dx, dy);
        if (action == MotionEvent.ACTION_CANCEL) pointers.clear();
        else if (event.getActionMasked() == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP) {
            pointers.remove(index);
        }
        boolean handled = current.dispatchTouchEvent(event);
        finishIfRequested();
        return handled;
    }

    public static boolean dispatchKey(long time, int action, int code, int meta) {
        if (action == KeyEvent.ACTION_DOWN) keyDownTime = time;
        return dispatchSystemKey(new KeyEvent(keyDownTime, time, action, code, 0, meta));
    }

    public static boolean dispatchSystemKey(KeyEvent event) {
        if (current == null || !current.isResumed()) return false;
        if (event.getKeyCode() == KeyEvent.KEYCODE_HOME) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) sendHome();
            return true;
        }
        boolean handled = current.dispatchKeyEvent(event);
        finishIfRequested();
        return handled;
    }

    public static boolean dispatchIme(String text, boolean composing) {
        if (current == null || !current.isResumed()) return false;
        boolean handled = current.dispatchImeText(text, composing);
        finishIfRequested();
        return handled;
    }

    private static native Activity nativeCreateActivity(String name);
}
