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

    public LauncherApps(Context context) { packages = context.getPackageManager(); }
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
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
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
