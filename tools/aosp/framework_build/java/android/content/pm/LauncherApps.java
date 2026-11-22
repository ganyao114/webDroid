package android.content.pm;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.UserHandle;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.Executor;

public class LauncherApps {
    private final PackageManager packages;
    private final ArrayList<Registration> callbacks = new ArrayList<>();
    private static final class Registration {
        final Callback callback;
        final Handler handler;
        Registration(Callback callback, Handler handler) {
            this.callback = callback;
            this.handler = handler;
        }
    }

    public abstract static class Callback {
        public abstract void onPackageRemoved(String name, UserHandle user);
        public abstract void onPackageAdded(String name, UserHandle user);
        public abstract void onPackageChanged(String name, UserHandle user);
        public abstract void onPackagesAvailable(String[] names, UserHandle user, boolean replacing);
        public abstract void onPackagesUnavailable(String[] names, UserHandle user, boolean replacing);
    }

    public static class ShortcutQuery {
        public static final int FLAG_MATCH_DYNAMIC = 1, FLAG_MATCH_PINNED = 2, FLAG_MATCH_MANIFEST = 8;
        private int flags;
        private String packageName;
        private List<String> shortcutIds;
        private ComponentName activity;

        public ShortcutQuery setQueryFlags(int flags) { this.flags = flags; return this; }
        public ShortcutQuery setPackage(String name) { packageName = name; return this; }
        public ShortcutQuery setShortcutIds(List<String> ids) { shortcutIds = ids; return this; }
        public ShortcutQuery setActivity(ComponentName component) { activity = component; return this; }
        public int getQueryFlags() { return flags; }
        public String getPackage() { return packageName; }
        public List<String> getShortcutIds() { return shortcutIds; }
        public ComponentName getActivity() { return activity; }
    }

    public LauncherApps(Context context) { packages = context.getPackageManager(); }
    public LauncherUserInfo getLauncherUserInfo(UserHandle user) {
        return UserHandle.SYSTEM.equals(user)
                ? new LauncherUserInfo.Builder("android.os.usertype.full.SYSTEM", 0).build() : null;
    }
    public List<String> getPreInstalledSystemPackages(UserHandle user) {
        return java.util.Collections.emptyList();
    }
    public boolean hasShortcutHostPermission() { return false; }

    public List<ShortcutInfo> getShortcuts(ShortcutQuery query, UserHandle user) {
        if (query.getShortcutIds() != null && query.getPackage() == null) {
            throw new IllegalArgumentException("Shortcut IDs require a package");
        }
        Objects.requireNonNull(user);
        // The host catalog has launchable APK activities and no pinned or dynamic shortcuts.
        return java.util.Collections.emptyList();
    }
    public List<PackageInstaller.SessionInfo> getAllPackageInstallerSessions() {
        return packages.getPackageInstaller().getAllSessions();
    }
    public void registerPackageInstallerSessionCallback(Executor executor,
            PackageInstaller.SessionCallback callback) {
        Objects.requireNonNull(executor);
        // APK publication is atomic in the host and produces no guest session events.
        packages.getPackageInstaller().registerSessionCallback(callback, null);
    }
    public void unregisterPackageInstallerSessionCallback(PackageInstaller.SessionCallback callback) {
        packages.getPackageInstaller().unregisterSessionCallback(callback);
    }
    public void registerCallback(Callback callback) { registerCallback(callback, null); }
    public void registerCallback(Callback callback, Handler handler) {
        if (callback == null) throw new NullPointerException("callback");
        unregisterCallback(callback);
        callbacks.add(new Registration(callback, handler == null ? new Handler(Looper.getMainLooper()) : handler));
    }
    public void unregisterCallback(Callback callback) {
        for (int i = callbacks.size() - 1; i >= 0; --i) {
            if (callbacks.get(i).callback == callback) callbacks.remove(i);
        }
    }
    public void dispatchPackageChanged(String name, boolean replaced) {
        for (Registration registration : callbacks) {
            registration.handler.post(() -> {
                if (!callbacks.contains(registration)) return;
                if (replaced) registration.callback.onPackageChanged(name, UserHandle.SYSTEM);
                else registration.callback.onPackageAdded(name, UserHandle.SYSTEM);
            });
        }
    }
    public List<LauncherActivityInfo> getActivityList(String packageName, UserHandle user) {
        return queryActivities(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), packageName, user);
    }
    public List<LauncherActivityInfo> getShortcutConfigActivityList(String packageName, UserHandle user) {
        return queryActivities(new Intent(Intent.ACTION_CREATE_SHORTCUT), packageName, user);
    }
    private List<LauncherActivityInfo> queryActivities(Intent intent, String packageName, UserHandle user) {
        if (packageName != null) intent.setPackage(packageName);
        List<ResolveInfo> matches = packages.queryIntentActivitiesAsUser(intent, 0, user.getIdentifier());
        ArrayList<LauncherActivityInfo> result = new ArrayList<>(matches.size());
        for (ResolveInfo match : matches) result.add(new LauncherActivityInfo(packages, match.activityInfo, user));
        return result;
    }
    public LauncherActivityInfo resolveActivity(Intent intent, UserHandle user) {
        List<ResolveInfo> result = packages.queryIntentActivitiesAsUser(intent, 0, user.getIdentifier());
        return result.isEmpty() ? null : new LauncherActivityInfo(packages, result.get(0).activityInfo, user);
    }
    public ApplicationInfo getApplicationInfo(String pkg, int flags, UserHandle user)
            throws PackageManager.NameNotFoundException {
        return packages.getApplicationInfoAsUser(pkg, flags, user.getIdentifier());
    }
    public boolean isPackageEnabled(String pkg, UserHandle user) {
        try { return getApplicationInfo(pkg, 0, user).enabled; }
        catch (PackageManager.NameNotFoundException error) { return false; }
    }
    public boolean isActivityEnabled(ComponentName component, UserHandle user) {
        LauncherActivityInfo info = resolveActivity(new Intent().setComponent(component), user);
        return info != null && info.getActivityInfo().isEnabled();
    }
    public void startMainActivity(ComponentName component, UserHandle user, Rect sourceBounds, Bundle options) {
        packages.launch(Intent.makeMainActivity(component).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), user, options);
    }
}
