package android.content.pm;

import android.content.ComponentName;
import android.graphics.drawable.Drawable;
import android.os.UserHandle;

public final class LauncherActivityInfo {
    private final PackageManager pm;
    private final ActivityInfo activity;
    private final UserHandle user;
    LauncherActivityInfo(PackageManager pm, ActivityInfo activity, UserHandle user) {
        this.pm = pm; this.activity = activity; this.user = user;
    }
    public ComponentName getComponentName() { return new ComponentName(activity.packageName, activity.name); }
    public UserHandle getUser() { return user; }
    public CharSequence getLabel() { return activity.loadLabel(pm); }
    public Drawable getIcon(int density) {
        int id = activity.getIconResource();
        Drawable result = id == 0 ? null : pm.getDrawableForDensity(activity.packageName, id, density,
                activity.applicationInfo);
        return result == null ? pm.getDefaultActivityIcon() : result;
    }
    public Drawable getBadgedIcon(int density) { return getIcon(density); }
    public ApplicationInfo getApplicationInfo() { return activity.applicationInfo; }
    public ActivityInfo getActivityInfo() { return activity; }
    public String getName() { return activity.name; }
}
