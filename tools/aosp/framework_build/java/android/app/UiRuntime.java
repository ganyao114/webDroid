package android.app;

import android.appwidget.AppWidgetManager;
import android.content.ClipboardManager;
import android.content.ComponentName;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.LauncherApps;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.LocaleList;
import android.os.PowerManager;
import android.os.SystemClock;
import android.os.UserHandle;
import android.os.UserManager;
import android.os.storage.StorageManager;
import android.util.ArrayMap;
import android.view.Choreographer;
import android.view.Display;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.WindowManager;
import android.view.WindowMetrics;
import com.android.internal.policy.PhoneLayoutInflater;
import com.android.internal.policy.PhoneWindow;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.wasmandroid.ui.XmlValues;

/** Entry points invoked by UiBridge on the interpreter thread. */
public final class UiRuntime {
    private static Activity current;
    private static AppContext context;
    private static ViewRootImpl root;
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
        final ContentResolver resolver;
        private LayoutInflater inflater;
        private LauncherApps launcher;
        private WallpaperManager wallpaper;
        private SearchManager search;
        private android.view.inputmethod.InputMethodManager inputMethod;
        private android.view.accessibility.AccessibilityManager accessibility;
        private ClipboardManager clipboard;
        private PowerManager power;
        private BatteryManager battery;
        private UiModeManager uiMode;
        private KeyguardManager keyguard;
        private NotificationManager notifications;
        private StorageManager storage;
        private ActivityManager activities;
        private AlarmManager alarms;
        private android.media.AudioManager audio;
        private android.net.ConnectivityManager connectivity;
        private Resources.Theme theme;
        private final ArrayMap<String, ServiceInfo> services;
        private final ArrayList<String> manifestReceivers;
        private final ArrayMap<ComponentName, RunningService> runningServices = new ArrayMap<>();
        private final ArrayMap<ServiceConnection, RunningService> connections = new ArrayMap<>();
        private boolean closing;
        private boolean interactive = true;

        private static final class RunningService {
            final Service instance;
            final ComponentName name;
            int nextStartId;
            boolean started;
            int clients;
            Intent bindIntent;
            IBinder binding;
            boolean rebind;

            RunningService(Service instance, ComponentName name) {
                this.instance = instance;
                this.name = name;
            }
        }

        AppContext(Resources resources, ApplicationInfo info, Manifest manifest) {
            this.resources = resources; this.info = info;
            services = manifest.services;
            manifestReceivers = manifest.receivers;
            packages = PackageManager.forHost(this, services, manifest.packageInfo);
            resolver = new ContentResolver(this) {};
            displays = new DisplayManager(new Display(resources));
            windows = new WindowManager() {
                @Override public Display getDefaultDisplay() { return getDisplay(); }
                @Override public WindowMetrics getCurrentWindowMetrics() { return PhoneWindow.displayMetrics(AppContext.this); }
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
        @Override public int getDisplayId() { return getDisplay().getDisplayId(); }
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
            closing = true;
            try {
                for (RunningService service : new ArrayList<>(runningServices.values())) {
                    try {
                        destroyService(service);
                    } catch (RuntimeException error) {
                        android.util.Log.e("UiRuntime", "Service shutdown failed: " + service.name, error);
                    }
                }
            } finally {
                try {
                    connections.clear();
                    if (clipboard != null) clipboard.detachEmbedded();
                    if (notifications != null) notifications.cancelAll();
                    PendingIntent.cancelEmbeddedForContext(getApplicationContext());
                } finally {
                    releaseResources();
                }
            }
        }
        private void releaseResources() {
            receivers.clear();
            for (Resources configured : configuredResources) configured.close();
            configuredResources.clear();
            packages.closePackageResources();
            resources.close();
        }
        @Override public SharedPreferences getSharedPreferences(String name, int mode) {
            synchronized (packagePreferences) {
                return packagePreferences.computeIfAbsent(info.packageName, key -> new android.util.ArrayMap<>())
                        .computeIfAbsent(name, key -> new SharedPreferencesImpl(
                                info.targetSdkVersion >= Build.VERSION_CODES.R));
            }
        }
        @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                                 Handler handler, int flags) {
            Intent batteryState = filter.hasAction(Intent.ACTION_BATTERY_CHANGED)
                    ? new Intent(Intent.ACTION_BATTERY_CHANGED)
                            .putExtra(BatteryManager.EXTRA_LEVEL, 100)
                            .putExtra(BatteryManager.EXTRA_SCALE, 100)
                            .putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_FULL)
                            .putExtra(BatteryManager.EXTRA_PLUGGED, BatteryManager.BATTERY_PLUGGED_AC)
                            .putExtra(BatteryManager.EXTRA_PRESENT, true)
                    : null;
            if (receiver != null && (permission == null || permission.isEmpty())) {
                ReceiverRegistration registration = new ReceiverRegistration(receiver, filter,
                        handler == null ? new Handler(getMainLooper()) : handler);
                receivers.add(registration);
                if (batteryState != null) {
                    registration.handler.post(() -> {
                        if (receivers.contains(registration)) deliverBroadcast(receiver, new Intent(batteryState), true);
                    });
                }
            }
            return batteryState;
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
            Intent copy = new Intent(intent);
            ComponentName component = copy.getComponent();
            if (component != null) {
                if (component.getPackageName().equals(getPackageName())
                        && manifestReceivers.contains(component.getClassName())) {
                    new Handler(getMainLooper()).post(() -> {
                        if (!closing) deliverBroadcast(
                                instantiate(component.getClassName(), BroadcastReceiver.class), copy, false);
                    });
                }
                return;
            }
            var data = intent.getData();
            String scheme = data == null ? null : data.getScheme();
            for (ReceiverRegistration registration : receivers) {
                if (registration.filter.match(intent.getAction(), intent.getType(), scheme,
                        data, intent.getCategories(), "BroadcastReceiver") < 0) continue;
                registration.handler.post(() -> {
                    if (!receivers.contains(registration)) return;
                    deliverBroadcast(registration.receiver, new Intent(copy), false);
                });
            }
        }
        @Override public void sendBroadcast(Intent intent, String permission) {
            if (permission == null || checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
                sendBroadcast(intent);
            }
        }
        private void deliverBroadcast(BroadcastReceiver receiver, Intent intent, boolean sticky) {
            var result = new BroadcastReceiver.PendingResult(0, null, null,
                    BroadcastReceiver.PendingResult.TYPE_REGISTERED, false, sticky, null, 0, 0);
            receiver.setPendingResult(result);
            try {
                receiver.onReceive(getApplicationContext(), intent);
            } finally {
                if (receiver.getPendingResult() == result) result.finish();
                receiver.setPendingResult(null);
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
        @Override public ComponentName startService(Intent intent) {
            RunningService service = getService(intent, true);
            if (service == null) return null;
            service.started = true;
            service.instance.onStartCommand(new Intent(intent), 0, ++service.nextStartId);
            return service.name;
        }
        @Override public ComponentName startForegroundService(Intent intent) { return startService(intent); }
        @Override public boolean stopService(Intent intent) {
            RunningService service = getService(intent, false);
            return service != null && stopService(service, -1);
        }
        @Override public boolean bindService(Intent intent, ServiceConnection connection, int flags) {
            if (connections.containsKey(connection)) return true;
            RunningService service = getService(intent, (flags & BIND_AUTO_CREATE) != 0);
            if (service == null) return false;
            if (service.bindIntent == null) {
                service.bindIntent = new Intent(intent);
                service.binding = service.instance.onBind(service.bindIntent);
            } else if (service.clients == 0 && service.rebind) {
                service.rebind = false;
                service.instance.onRebind(new Intent(intent));
            }
            service.clients++;
            connections.put(connection, service);
            new Handler(getMainLooper()).post(() -> {
                if (connections.get(connection) != service) return;
                if (service.binding == null) connection.onNullBinding(service.name);
                else connection.onServiceConnected(service.name, service.binding);
            });
            return true;
        }
        @Override public void unbindService(ServiceConnection connection) {
            RunningService service = connections.remove(connection);
            if (service == null) throw new IllegalArgumentException("Service connection is not bound");
            if (--service.clients == 0) {
                service.rebind = service.instance.onUnbind(service.bindIntent);
                if (!service.started) destroyService(service);
            }
        }
        private RunningService getService(Intent intent, boolean create) {
            ComponentName name = intent.getComponent();
            if (closing || name == null || !getPackageName().equals(name.getPackageName())) return null;
            RunningService service = runningServices.get(name);
            if (service != null || !create) return service;
            ServiceInfo info = services.get(name.getClassName());
            if (info == null || !this.info.processName.equals(info.processName)) return null;
            Service instance = instantiate(name.getClassName(), Service.class);
            RunningService record = new RunningService(instance, name);
            instance.attachEmbedded(this, name.getClassName(), application, info.getForegroundServiceType(),
                    startId -> stopService(record, startId));
            runningServices.put(name, record);
            try {
                instance.onCreate();
            } catch (RuntimeException | Error failure) {
                runningServices.remove(name);
                instance.detachAndCleanUp();
                throw failure;
            }
            return record;
        }
        private boolean stopService(RunningService service, int startId) {
            if (!service.started || (startId != -1 && startId != service.nextStartId)) return false;
            service.started = false;
            if (service.clients == 0) destroyService(service);
            return true;
        }
        private void destroyService(RunningService service) {
            if (runningServices.get(service.name) != service) return;
            runningServices.remove(service.name);
            service.started = false;
            try {
                service.instance.onDestroy();
            } finally {
                service.instance.detachAndCleanUp();
            }
        }
        private static File ensureDirectory(File directory) {
            if (!directory.isDirectory()) directory.mkdirs();
            return directory;
        }
        private File dataDirectory(String name) {
            return ensureDirectory(new File(getDataDir(), name));
        }
        private static File childFile(File directory, String name) {
            if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0) {
                throw new IllegalArgumentException("Invalid file name " + name);
            }
            return new File(directory, name);
        }
        @Override public File getDataDir() { return ensureDirectory(super.getDataDir()); }
        @Override public File getFilesDir() { return dataDirectory("files"); }
        @Override public File getCacheDir() { return dataDirectory("cache"); }
        @Override public File getCodeCacheDir() { return dataDirectory("code_cache"); }
        @Override public File getNoBackupFilesDir() { return dataDirectory("no_backup"); }
        @Override public File getDir(String name, int mode) {
            return ensureDirectory(childFile(getDataDir(), "app_" + name));
        }
        @Override public File getFileStreamPath(String name) { return childFile(getFilesDir(), name); }
        @Override public FileInputStream openFileInput(String name) throws FileNotFoundException {
            return new FileInputStream(getFileStreamPath(name));
        }
        @Override public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
            return new FileOutputStream(getFileStreamPath(name), (mode & MODE_APPEND) != 0);
        }
        @Override public boolean deleteFile(String name) { return getFileStreamPath(name).delete(); }
        @Override public File getDatabasePath(String name) {
            File file = new File(name);
            if (file.isAbsolute()) {
                File parent = file.getParentFile();
                if (parent != null) ensureDirectory(parent);
                return file;
            }
            return childFile(dataDirectory("databases"), name);
        }
        @Override public File getExternalFilesDir(String type) { return externalDirectory("data", "files", type); }
        @Override public File[] getExternalFilesDirs(String type) { return new File[] {getExternalFilesDir(type)}; }
        @Override public File getExternalCacheDir() { return externalDirectory("data", "cache", null); }
        @Override public File[] getExternalCacheDirs() { return new File[] {getExternalCacheDir()}; }
        @Override public File[] getExternalMediaDirs() { return new File[] {externalDirectory("media", null, null)}; }
        @Override public File getObbDir() { return externalDirectory("obb", null, null); }
        @Override public File[] getObbDirs() { return new File[] {getObbDir()}; }
        private File externalDirectory(String area, String directory, String type) {
            File path = StorageManager.getEmbeddedPackageDirectory(getPackageName(), area);
            if (directory != null) path = new File(path, directory);
            if (type != null) {
                if (type.isEmpty() || type.equals(".") || type.equals("..") || type.indexOf('/') >= 0) {
                    throw new IllegalArgumentException("Invalid storage directory name " + type);
                }
                path = new File(path, type);
            }
            return ensureDirectory(path);
        }
        void setInteractive(boolean interactive) {
            this.interactive = interactive;
            if (power != null) power.setEmbeddedInteractive(interactive);
        }
        @Override public void applyEmbeddedUiMode(int mode) {
            Configuration configuration = new Configuration(resources.getConfiguration());
            if (configuration.uiMode == mode) return;
            configuration.uiMode = mode;
            resources.updateConfiguration(configuration, resources.getDisplayMetrics());
            for (Resources configured : configuredResources) {
                Configuration override = new Configuration(configured.getConfiguration());
                override.uiMode = mode;
                configured.updateConfiguration(override, configured.getDisplayMetrics());
            }
            if (application != null) application.onConfigurationChanged(configuration);
            for (RunningService service : new ArrayList<>(runningServices.values())) {
                service.instance.onConfigurationChanged(configuration);
            }
            if (context == this && current != null) {
                current.onConfigurationChanged(configuration);
                current.getWindow().getDecorView().requestLayout();
            }
        }
        @Override public String getSystemServiceName(Class<?> type) {
            return type == android.media.AudioManager.class ? AUDIO_SERVICE : super.getSystemServiceName(type);
        }

        @Override public Object getSystemService(String name) {
            if (DISPLAY_SERVICE.equals(name)) return displays;
            if (WINDOW_SERVICE.equals(name)) return windows;
            if (ACTIVITY_SERVICE.equals(name)) {
                if (activities == null) activities = new ActivityManager(this, new Handler(getMainLooper()));
                return activities;
            }
            if (ALARM_SERVICE.equals(name)) {
                if (alarms == null) alarms = new AlarmManager(this);
                return alarms;
            }
            if (AUDIO_SERVICE.equals(name)) {
                if (audio == null) audio = new android.media.AudioManager(this);
                return audio;
            }
            if (USER_SERVICE.equals(name)) return users;
            if (APPWIDGET_SERVICE.equals(name)) return widgets;
            if (CONNECTIVITY_SERVICE.equals(name)) {
                if (connectivity == null) connectivity = new android.net.ConnectivityManager(this);
                return connectivity;
            }
            if (CLIPBOARD_SERVICE.equals(name)) {
                if (clipboard == null) clipboard = new ClipboardManager(this, new Handler(getMainLooper()));
                return clipboard;
            }
            if (POWER_SERVICE.equals(name)) {
                if (power == null) {
                    power = new PowerManager(this, new Handler(getMainLooper()));
                    power.setEmbeddedInteractive(interactive);
                }
                return power;
            }
            if (BATTERY_SERVICE.equals(name)) {
                if (battery == null) battery = new BatteryManager(this);
                return battery;
            }
            if (UI_MODE_SERVICE.equals(name)) {
                if (uiMode == null) uiMode = new UiModeManager(this);
                return uiMode;
            }
            if (KEYGUARD_SERVICE.equals(name)) {
                if (keyguard == null) keyguard = new KeyguardManager(this);
                return keyguard;
            }
            if (NOTIFICATION_SERVICE.equals(name)) {
                if (notifications == null) notifications = new NotificationManager(this);
                return notifications;
            }
            if (STORAGE_SERVICE.equals(name)) {
                if (storage == null) storage = new StorageManager(this, getMainLooper());
                return storage;
            }
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
                if (inputMethod == null) {
                    inputMethod = new android.view.inputmethod.InputMethodManager(
                            getDisplay().getDisplayId());
                }
                return inputMethod;
            }
            if (WALLPAPER_SERVICE.equals(name)) {
                if (wallpaper == null) wallpaper = new WallpaperManager(this);
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
        final PackageInfo packageInfo = new PackageInfo();
        final ArrayMap<String, ServiceInfo> services = new ArrayMap<>();
        final ArrayList<String> receivers = new ArrayList<>();
        String application;
        String activity;
        int theme;
    }

    public static void launch(String apkPath, String requested, String locale, String frameworkApk,
                              String nativeLibraryDir, int density, int width, int height)
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
            manifest.info.nativeLibraryDir = nativeLibraryDir;
            AppContext app = new AppContext(resources, manifest.info, manifest);
            context = app;
            Application.setEmbeddedProcessName(manifest.info.processName);
            ActivityThread.bindEmbeddedProcess(manifest.info);
            app.application = createApplication(app, manifest.application);
            ActivityThread.currentActivityThread().mInitialApplication = app.application;
            if (Boolean.getBoolean("org.wasmandroid.foreground")) {
                app.application.getPackageManager().installEmbeddedProviders(app.application);
            }
            app.application.onCreate();
            Activity activity = nativeCreateActivity(manifest.activity);
            ComponentName component = new ComponentName(manifest.info.packageName, manifest.activity);
            activity.attachEmbedded(app, Intent.makeMainActivity(component), manifest.theme);
            PhoneWindow window = activity.attachEmbeddedWindow();
            try (TypedArray attrs = activity.obtainStyledAttributes(
                    new int[] {android.R.attr.windowBackground})) {
                if (attrs.hasValue(0)) window.setBackgroundDrawable(attrs.getDrawable(0));
            }
            root = new ViewRootImpl(activity, activity.getDisplay(), window);
            root.setView(window.getDecorView(), window.getAttributes(), null);
            current = activity;
            ActivityThread.updateEmbeddedTask(activity, activity, 1);
            activity.performCreate(null);
            resume();
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
        Application application = name == null ? new Application() : instantiate(name, Application.class);
        application.attach(context);
        return application;
    }

    private static <T> T instantiate(String name, Class<T> type) {
        try {
            return type.cast(Class.forName(name).getDeclaredConstructor().newInstance());
        } catch (ReflectiveOperationException error) {
            throw new IllegalArgumentException("Cannot instantiate " + name, error);
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
            result.info.processName = result.info.packageName;
            int versionCode = xml.getAttributeIntValue(XmlValues.ANDROID, "versionCode", 0);
            int versionCodeMajor = xml.getAttributeIntValue(XmlValues.ANDROID, "versionCodeMajor", 0);
            result.packageInfo.setLongVersionCode(PackageInfo.composeLongVersionCode(versionCodeMajor, versionCode));
            int versionName = xml.getAttributeResourceValue(XmlValues.ANDROID, "versionName", 0);
            result.packageInfo.versionName = versionName == 0
                    ? xml.getAttributeValue(XmlValues.ANDROID, "versionName") : res.getString(versionName);
            result.info.setVersionCode(result.packageInfo.getLongVersionCode());
            result.info.sourceDir = result.info.publicSourceDir = path;
            String desired = requested == null || requested.length() == 0 ? null : fullName(result.info.packageName, requested);
            String fallback = null;
            int fallbackTheme = 0;
            for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
                if (event != XmlPullParser.START_TAG) continue;
                if (xml.getDepth() == 2 && "application".equals(xml.getName())) {
                    String application = xml.getAttributeValue(XmlValues.ANDROID, "name");
                    if (application != null) result.application = fullName(result.info.packageName, application);
                    String process = xml.getAttributeValue(XmlValues.ANDROID, "process");
                    if (process != null && !process.isEmpty()) {
                        result.info.processName = process.startsWith(":") ? result.info.packageName + process : process;
                    }
                    result.info.icon = xml.getAttributeResourceValue(XmlValues.ANDROID, "icon", 0);
                    if (xml.getAttributeBooleanValue(XmlValues.ANDROID, "supportsRtl", false)) {
                        result.info.flags |= ApplicationInfo.FLAG_SUPPORTS_RTL;
                    }
                    result.info.theme = xml.getAttributeResourceValue(XmlValues.ANDROID, "theme", 0);
                    result.info.labelRes = xml.getAttributeResourceValue(XmlValues.ANDROID, "label", 0);
                    if (result.info.labelRes == 0) result.info.nonLocalizedLabel = xml.getAttributeValue(XmlValues.ANDROID, "label");
                } else if (xml.getDepth() == 2 && "uses-sdk".equals(xml.getName())) {
                    result.info.targetSdkVersion = xml.getAttributeIntValue(XmlValues.ANDROID, "targetSdkVersion", 37);
                } else if (xml.getDepth() == 3 && xml.getAttributeBooleanValue(XmlValues.ANDROID, "enabled", true)) {
                    String tag = xml.getName();
                    String name = xml.getAttributeValue(XmlValues.ANDROID, "name");
                    if (name != null && "meta-data".equals(tag)) {
                        result.info.metaData = readMetadata(res, xml, result.info.metaData);
                        continue;
                    }
                    if (name != null && "service".equals(tag)) {
                        ServiceInfo service = readService(res, xml, result.info, name);
                        result.services.put(service.name, service);
                    } else if (name != null && "receiver".equals(tag)) {
                        result.receivers.add(fullName(result.info.packageName, name));
                    }
                    if (!"activity".equals(tag)) continue;
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
    private static ServiceInfo readService(Resources res, XmlResourceParser xml, ApplicationInfo application,
                                           String name) throws IOException, XmlPullParserException {
        ServiceInfo service = new ServiceInfo();
        service.applicationInfo = application;
        service.packageName = application.packageName;
        service.name = fullName(application.packageName, name);
        String process = xml.getAttributeValue(XmlValues.ANDROID, "process");
        service.processName = process == null || process.isEmpty() ? application.processName
                : process.startsWith(":") ? application.packageName + process : process;
        service.exported = xml.getAttributeBooleanValue(XmlValues.ANDROID, "exported", false);
        service.permission = xml.getAttributeValue(XmlValues.ANDROID, "permission");
        service.mForegroundServiceType = xml.getAttributeIntValue(XmlValues.ANDROID, "foregroundServiceType", 0);
        int depth = xml.getDepth();
        for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
            if (event == XmlPullParser.END_TAG && xml.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG || xml.getDepth() != depth + 1 || !"meta-data".equals(xml.getName())) {
                continue;
            }
            service.metaData = readMetadata(res, xml, service.metaData);
        }
        return service;
    }
    private static Bundle readMetadata(Resources res, XmlResourceParser xml, Bundle metadata)
            throws XmlPullParserException {
        if (metadata == null) metadata = new Bundle();
        int resource = xml.getAttributeResourceValue(XmlValues.ANDROID, "resource", 0);
        if (resource != 0) {
            metadata.putInt(xml.getAttributeValue(XmlValues.ANDROID, "name"), resource);
        } else {
            res.parseBundleExtra("meta-data", xml, metadata);
        }
        return metadata;
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
    public static void sendBack() {
        if (current == null) return;
        PhoneWindow window = (PhoneWindow) current.getWindow();
        long now = SystemClock.uptimeMillis();
        if (window.dispatchEmbeddedKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0))) {
            window.dispatchEmbeddedKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
        } else {
            current.performBackPressed();
        }
        finishIfRequested();
    }
    public static void sendAllApps() {
        if (current != null) current.onNewIntent(new Intent(Intent.ACTION_ALL_APPS)
                .setComponent(current.getComponentName()));
    }
    public static void packageChanged(String name, boolean replaced) {
        if (context != null && context.launcher != null) context.launcher.dispatchPackageChanged(name, replaced);
    }
    public static void pause() {
        if (context != null) context.setInteractive(false);
        pauseActivity(current, root);
        finishIfRequested();
    }
    private static void pauseActivity(Activity activity, ViewRootImpl windowRoot) {
        if (activity == null || !activity.isResumed()) return;
        windowRoot.setWindowFocus(false);
        activity.onWindowFocusChanged(false);
        activity.performPause();
    }
    public static void stop() {
        if (context != null) context.setInteractive(false);
        pauseActivity(current, root);
        if (current != null) current.performStop(false, "host stop");
        finishIfRequested();
    }
    public static void resume() {
        if (context != null) context.setInteractive(true);
        if (current != null && !current.isResumed() && !current.isFinishing()) {
            current.performResume(false, "host resume");
            if (current != null && current.isResumed() && !current.isFinishing()) {
                root.setWindowFocus(true);
                current.onWindowFocusChanged(true);
            }
        }
        finishIfRequested();
    }
    public static void destroy() {
        Activity old = current;
        AppContext app = context;
        ViewRootImpl oldRoot = root;
        current = null;
        context = null;
        root = null;
        pointers.clear();
        try {
            if (old != null) {
                pauseActivity(old, oldRoot);
                old.performDestroy();
            }
        } finally {
            try {
                if (oldRoot != null) oldRoot.detach();
            } finally {
                if (app != null) {
                    try {
                        app.closeResources();
                    } finally {
                        if (context == null) {
                            ActivityThread.unbindEmbeddedProcess();
                            Application.setEmbeddedProcessName(null);
                        }
                    }
                }
            }
        }
    }
    private static void finishIfRequested() { if (current != null && current.isFinishing()) destroy(); }

    private static int frameWidth, frameHeight, frameDensity;
    private static boolean drew, forceDraw;
    private static final Runnable TRAVERSE = () -> {
        if (current != null && current.isResumed() && !current.isFinishing())
            drew = root.doFrame(frameWidth, frameHeight, frameDensity, forceDraw);
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
            boolean handled = root.dispatchHostInputEvent(event);
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
        boolean handled = root.dispatchHostInputEvent(event);
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
        boolean handled = root.dispatchHostInputEvent(event);
        finishIfRequested();
        return handled;
    }

    public static boolean dispatchIme(String text, boolean composing) {
        if (current == null || !current.isResumed()) return false;
        boolean handled = root.dispatchImeText(text, composing);
        finishIfRequested();
        return handled;
    }

    private static native Activity nativeCreateActivity(String name);
}
