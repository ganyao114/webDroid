package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.LauncherApps;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import android.os.Looper;
import android.os.LocaleList;
import android.os.UserHandle;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
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
        final Resources resources;
        final ApplicationInfo info;
        final PackageManager packages;
        final android.content.ContentResolver resolver = new android.content.ContentResolver();
        private LayoutInflater inflater;
        private LauncherApps launcher;
        private WallpaperManager wallpaper;
        private android.view.inputmethod.InputMethodManager inputMethod;
        private android.view.accessibility.AccessibilityManager accessibility;
        private Resources.Theme theme;

        AppContext(Resources resources, ApplicationInfo info) {
            this.resources = resources; this.info = info;
            packages = new PackageManager(this);
        }
        @Override public AssetManager getAssets() { return resources.getAssets(); }
        @Override public Resources getResources() { return resources; }
        @Override public String getPackageName() { return info.packageName; }
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
            if (LAYOUT_INFLATER_SERVICE.equals(name)) {
                if (inflater == null) inflater = new LayoutInflater(this);
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
            Activity activity = nativeCreateActivity(manifest.activity);
            AppContext app = new AppContext(resources, manifest.info);
            ComponentName component = new ComponentName(manifest.info.packageName, manifest.activity);
            activity.attach(app, Intent.makeMainActivity(component), manifest.theme);
            current = activity;
            context = app;
            activity.performCreate(null);
            finishIfRequested();
        } catch (IOException | XmlPullParserException | RuntimeException | Error error) {
            if (current != null) destroy();
            else resources.close();
            throw error;
        }
    }

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
                    result.info.icon = xml.getAttributeResourceValue(XmlValues.ANDROID, "icon", 0);
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
    public static void sendBack() { if (current != null) current.onBackPressed(); }
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
            if (app != null) app.resources.close();
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
                current.onConfigurationChanged(context.resources.getConfiguration());
            }
        }
        looper.dispatchDueMessages(nanos / 1_000_000);
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
        if (current == null || !current.isResumed()) return false;
        if (action == KeyEvent.ACTION_DOWN) keyDownTime = time;
        boolean handled = current.dispatchKeyEvent(new KeyEvent(keyDownTime, time, action, code, 0, meta));
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
