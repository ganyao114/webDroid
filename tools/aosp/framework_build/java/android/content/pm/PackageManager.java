package android.content.pm;

import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.res.Resources;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.UserHandle;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class PackageManager {
    public static final int GET_META_DATA = 0x80, MATCH_DEFAULT_ONLY = 0x10000, MATCH_DISABLED_COMPONENTS = 0x200;
    public static final int COMPONENT_ENABLED_STATE_DEFAULT = 0, COMPONENT_ENABLED_STATE_ENABLED = 1;
    private final Context context;
    public PackageManager(Context context) { this.context = context; }
    public static class NameNotFoundException extends Exception {
        public NameNotFoundException() {}
        public NameNotFoundException(String name) { super(name); }
    }
    public static final class ResolveInfoFlags {
        private final long value;
        private ResolveInfoFlags(long value) { this.value = value; }
        public static ResolveInfoFlags of(long value) { return new ResolveInfoFlags(value); }
        public long getValue() { return value; }
    }
    public static final class ApplicationInfoFlags {
        private final long value;
        private ApplicationInfoFlags(long value) { this.value = value; }
        public static ApplicationInfoFlags of(long value) { return new ApplicationInfoFlags(value); }
        public long getValue() { return value; }
    }
    private static final class Entry {
        final String pkg, activity, label, apk;
        final int icon;
        final boolean enabled;
        private Entry(String pkg, String activity, String label, String apk, int icon, boolean enabled) {
            this.pkg = pkg; this.activity = activity; this.label = label; this.apk = apk;
            this.icon = icon; this.enabled = enabled;
        }
        ApplicationInfo applicationInfo() {
            ApplicationInfo info = new ApplicationInfo();
            info.packageName = pkg;
            info.sourceDir = info.publicSourceDir = apk;
            info.nonLocalizedLabel = label;
            info.icon = icon;
            info.enabled = enabled;
            return info;
        }
        ResolveInfo resolveInfo() {
            ResolveInfo result = new ResolveInfo();
            ActivityInfo info = new ActivityInfo();
            info.applicationInfo = applicationInfo();
            info.packageName = pkg; info.name = activity; info.enabled = enabled; info.exported = true;
            result.activityInfo = info;
            return result;
        }
    }

    public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) {
        return queryIntentActivitiesAsUser(intent, flags, UserHandle.USER_SYSTEM);
    }
    public List<ResolveInfo> queryIntentActivities(Intent intent, ResolveInfoFlags flags) {
        return queryIntentActivitiesAsUser(intent, flags.getValue(), UserHandle.USER_SYSTEM);
    }
    public List<ResolveInfo> queryIntentActivitiesAsUser(Intent intent, int flags, int user) {
        return queryIntentActivitiesAsUser(intent, (long) flags, user);
    }
    private List<ResolveInfo> queryIntentActivitiesAsUser(Intent intent, long flags, int user) {
        ComponentName component = intent.getComponent();
        String pkg = component == null ? intent.getPackage() : component.getPackageName();
        String activity = component == null ? null : component.getClassName();
        Entry[] found = nativeQuery(intent.getAction(), pkg, activity, intent.getDataString(),
                intent.getType(), intent.categoryArray(), flags, user);
        ArrayList<ResolveInfo> result = new ArrayList<>(found.length);
        for (Entry entry : found) {
            if (entry.activity != null && (entry.enabled || (flags & MATCH_DISABLED_COMPONENTS) != 0)) {
                result.add(entry.resolveInfo());
            }
        }
        return result;
    }
    public ResolveInfo resolveActivity(Intent intent, int flags) { return resolveActivity(intent, ResolveInfoFlags.of(flags)); }
    public ResolveInfo resolveActivity(Intent intent, ResolveInfoFlags flags) {
        List<ResolveInfo> result = queryIntentActivities(intent, flags);
        return result.isEmpty() ? null : result.get(0);
    }
    public ApplicationInfo getApplicationInfo(String pkg, ApplicationInfoFlags flags) throws NameNotFoundException {
        return getApplicationInfoAsUser(pkg, flags.getValue(), UserHandle.USER_SYSTEM);
    }
    ApplicationInfo getApplicationInfoAsUser(String pkg, long flags, int user) throws NameNotFoundException {
        if (user == UserHandle.USER_SYSTEM && context.getPackageName().equals(pkg)) {
            return new ApplicationInfo(context.getApplicationInfo());
        }
        Entry info = nativeApplication(pkg, flags, user);
        if (info == null) throw new NameNotFoundException(pkg);
        return info.applicationInfo();
    }
    public ApplicationInfo getApplicationInfo(String pkg, int flags) throws NameNotFoundException {
        return getApplicationInfo(pkg, ApplicationInfoFlags.of(flags));
    }
    public CharSequence getApplicationLabel(ApplicationInfo info) { return info.loadLabel(this); }
    public Drawable getApplicationIcon(ApplicationInfo info) { return info.loadIcon(this); }
    public Drawable getApplicationIcon(String pkg) throws NameNotFoundException { return getApplicationIcon(getApplicationInfo(pkg, 0)); }
    public Drawable getActivityIcon(ComponentName component) throws NameNotFoundException {
        ResolveInfo info = resolveActivity(new Intent().setComponent(component), 0);
        if (info == null) throw new NameNotFoundException(component.flattenToString());
        return info.loadIcon(this);
    }
    public Drawable getActivityIcon(Intent intent) throws NameNotFoundException {
        ResolveInfo result = resolveActivity(intent, 0);
        if (result == null) throw new NameNotFoundException(intent.toString());
        return result.loadIcon(this);
    }
    public Intent getLaunchIntentForPackage(String pkg) {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(pkg);
        ResolveInfo resolve = resolveActivity(intent, 0);
        return resolve == null ? null : intent.setClassName(pkg, resolve.activityInfo.name).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }
    public Drawable getDefaultActivityIcon() {
        try {
            return context.getDrawable(android.R.drawable.sym_def_app_icon);
        } catch (Resources.NotFoundException unavailable) {
            return new ColorDrawable(0xff8b9299);
        }
    }

    public Drawable getDrawable(String pkg, int resId, ApplicationInfo optional) {
        return getDrawableForDensity(pkg, resId, 0, optional);
    }

    Drawable getDrawableForDensity(String pkg, int resId, int density, ApplicationInfo optional) {
        try {
            if (context.getPackageName().equals(pkg)) {
                return context.getResources().getDrawableForDensity(resId, density, context.getTheme());
            }
            ApplicationInfo info = optional == null ? getApplicationInfo(pkg, 0) : optional;
            if (info.sourceDir == null) return null;
            try (Resources res = Resources.forApk(info.sourceDir, context.getResources().getConfiguration().getLocaleTag(),
                    context.getResources().getDisplayMetrics().densityDpi)) {
                return res.getDrawableForDensity(resId, density, null);
            }
        } catch (NameNotFoundException | Resources.NotFoundException | IOException error) {
            return null;
        }
    }
    public CharSequence getText(String pkg, int resId, ApplicationInfo optional) {
        try {
            if (context.getPackageName().equals(pkg)) return context.getText(resId);
            ApplicationInfo info = optional == null ? getApplicationInfo(pkg, 0) : optional;
            if (info.sourceDir == null) return null;
            try (Resources res = Resources.forApk(info.sourceDir, context.getResources().getConfiguration().getLocaleTag(),
                    context.getResources().getDisplayMetrics().densityDpi)) {
                return res.getText(resId);
            }
        } catch (NameNotFoundException | Resources.NotFoundException | IOException error) {
            return null;
        }
    }
    public void launch(Intent intent, UserHandle user, Bundle options) {
        ComponentName component = intent.getComponent();
        String pkg = component == null ? intent.getPackage() : component.getPackageName();
        String cls = component == null ? null : component.getClassName();
        if (!nativeLaunch(intent.getAction(), pkg, cls, intent.getDataString(), intent.getType(),
                intent.categoryArray(), intent.getFlags(), user.getIdentifier())) {
            throw new ActivityNotFoundException(intent.toString());
        }
    }

    private static native Entry[] nativeQuery(String action, String pkg, String activity, String data,
                                               String mime, String[] categories, long flags, int user);
    private static native Entry nativeApplication(String pkg, long flags, int user);
    private static native boolean nativeLaunch(String action, String pkg, String activity, String data,
                                               String mime, String[] categories, int flags, int user);
}
