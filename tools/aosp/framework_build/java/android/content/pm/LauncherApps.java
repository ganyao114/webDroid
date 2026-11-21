package android.content.pm;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.UserHandle;
import java.util.ArrayList;
import java.util.List;

public class LauncherApps {
    private final PackageManager packages;
    public LauncherApps(Context context) { packages = context.getPackageManager(); }
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
