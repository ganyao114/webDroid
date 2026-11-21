package com.android.launcher3;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.LauncherActivityInfo;
import android.content.pm.LauncherApps;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.drawable.Drawable;
import android.os.Process;
import android.os.UserHandle;
import android.util.Log;
import com.android.launcher3.LauncherSettings.Favorites;
import com.android.launcher3.model.data.AppInfo;
import com.android.launcher3.model.data.WorkspaceItemInfo;
import java.io.IOException;
import java.net.URISyntaxException;
import java.text.Collator;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

final class AppCatalog {
    private static final String TAG = "Launcher3";
    private static final String NS = "http://schemas.android.com/apk/res-auto/com.android.launcher3";

    record App(AppInfo item, Drawable.ConstantState iconState) {
        Drawable icon(Context context) {
            return iconState == null ? context.getDrawable(R.drawable.ic_launcher_home)
                    : iconState.newDrawable(context.getResources(), context.getTheme());
        }
    }

    record Placement(WorkspaceItemInfo item, App app) {}
    record Snapshot(List<App> apps, List<Placement> defaults) {}

    static Snapshot load(Context context, DeviceProfile profile) {
        var launcher = context.getSystemService(LauncherApps.class);
        var packages = context.getPackageManager();
        var user = Process.myUserHandle();
        Map<ComponentName, App> apps = new LinkedHashMap<>();
        if (launcher != null) {
            try {
                for (LauncherActivityInfo activity : launcher.getActivityList(null, user)) {
                    ComponentName component = activity.getComponentName();
                    if (!component.getPackageName().equals(context.getPackageName())) {
                        apps.put(component, app(component, activity.getLabel(), activity.getBadgedIcon(0),
                                activity.getApplicationInfo(), user));
                    }
                }
            } catch (SecurityException denied) {
                Log.w(TAG, "LauncherApps unavailable for current user", denied);
            }
        }
        if (apps.isEmpty()) {
            Intent pick = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            for (ResolveInfo activity : packages.queryIntentActivities(pick, 0)) {
                ActivityInfo info = activity.activityInfo;
                if (info != null && !info.packageName.equals(context.getPackageName())) {
                    ComponentName component = new ComponentName(info.packageName, info.name);
                    apps.put(component, app(component, activity.loadLabel(packages), activity.loadIcon(packages),
                            info.applicationInfo, user));
                }
            }
        }
        List<App> sorted = new ArrayList<>(apps.values());
        Collator labels = Collator.getInstance(context.getResources().getConfiguration().getLocales().get(0));
        sorted.sort(Comparator.comparing((App entry) -> entry.item.title.toString(), labels)
                .thenComparing(entry -> entry.item.componentName.flattenToString()));

        return new Snapshot(sorted,
                defaults(context.getResources(), profile.workspaceLayout, apps, packages, profile));
    }

    private static App app(ComponentName component, CharSequence label, Drawable icon,
                           ApplicationInfo application, UserHandle user) {
        AppInfo info = new AppInfo();
        info.componentName = component;
        info.user = user;
        info.title = label != null ? label : component.getPackageName();
        info.contentDescription = info.title;
        info.intent = AppInfo.makeLaunchIntent(component);
        if (application != null) {
            info.uid = application.uid;
            if ((application.flags & ApplicationInfo.FLAG_SUSPENDED) != 0) {
                info.runtimeStatusFlags |= AppInfo.FLAG_DISABLED_SUSPENDED;
            }
        }
        return new App(info, icon == null ? null : icon.getConstantState());
    }

    private static List<Placement> defaults(Resources resources, int layout,
                                            Map<ComponentName, App> apps, PackageManager packages,
                                            DeviceProfile profile) {
        List<Placement> results = new ArrayList<>();
        try (XmlResourceParser xml = resources.getXml(layout)) {
            for (int event = xml.next(); event != XmlPullParser.END_DOCUMENT; event = xml.next()) {
                if (event != XmlPullParser.START_TAG || xml.getDepth() != 2) continue;
                int container = xml.getAttributeIntValue(NS, "container", Favorites.CONTAINER_DESKTOP);
                int screen = xml.getAttributeIntValue(NS, "screen", 0);
                int x = xml.getAttributeIntValue(NS, "x", 0);
                int y = xml.getAttributeIntValue(NS, "y", 0);
                App app = null;
                if ("favorite".equals(xml.getName())) {
                    app = resolve(xml, apps, packages);
                } else if ("resolve".equals(xml.getName())) {
                    while ((event = xml.next()) != XmlPullParser.END_DOCUMENT
                            && !(event == XmlPullParser.END_TAG && xml.getDepth() == 2)) {
                        if (app == null && event == XmlPullParser.START_TAG
                                && "favorite".equals(xml.getName())) {
                            app = resolve(xml, apps, packages);
                        }
                    }
                }
                if (app == null) continue;
                WorkspaceItemInfo item = app.item.makeWorkspaceItem(null);
                item.container = container;
                item.screenId = screen;
                item.cellX = container == Favorites.CONTAINER_HOTSEAT ? screen
                        : x < 0 ? profile.columns + x : x;
                item.cellY = container == Favorites.CONTAINER_HOTSEAT ? 0
                        : y < 0 ? profile.rows + y : y;
                results.add(new Placement(item, app));
            }
        } catch (IOException | XmlPullParserException failure) {
            throw new IllegalStateException("Reading Launcher3 default workspace", failure);
        }
        return results;
    }

    private static App resolve(XmlResourceParser xml, Map<ComponentName, App> apps, PackageManager pm) {
        String packageName = xml.getAttributeValue(NS, "packageName");
        String className = xml.getAttributeValue(NS, "className");
        if (packageName != null && className != null) {
            return apps.get(ComponentName.createRelative(packageName, className));
        }
        String uri = xml.getAttributeValue(NS, "uri");
        if (uri == null) return null;
        try {
            Intent intent = Intent.parseUri(uri, 0);
            ResolveInfo preferred = pm.resolveActivity(intent, PackageManager.MATCH_DEFAULT_ONLY);
            List<ResolveInfo> matches = pm.queryIntentActivities(intent, PackageManager.MATCH_DEFAULT_ONLY);
            if (preferred != null && preferred.activityInfo != null) {
                ActivityInfo target = preferred.activityInfo;
                for (ResolveInfo match : matches) {
                    if (target.packageName.equals(match.activityInfo.packageName)
                            && target.name.equals(match.activityInfo.name)) {
                        return mainActivity(apps, pm, target.packageName);
                    }
                }
            }
            String systemPackage = null;
            for (ResolveInfo match : matches) {
                ActivityInfo target = match.activityInfo;
                if ((target.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0) continue;
                if (systemPackage != null && !systemPackage.equals(target.packageName)) return null;
                systemPackage = target.packageName;
            }
            return systemPackage == null ? null : mainActivity(apps, pm, systemPackage);
        } catch (URISyntaxException invalidUri) {
            Log.w(TAG, "Invalid upstream workspace URI " + uri, invalidUri);
            return null;
        }
    }

    private static App mainActivity(Map<ComponentName, App> apps, PackageManager pm, String packageName) {
        Intent launch = pm.getLaunchIntentForPackage(packageName);
        if (launch != null && apps.containsKey(launch.getComponent())) {
            return apps.get(launch.getComponent());
        }
        for (var entry : apps.entrySet()) {
            if (entry.getKey().getPackageName().equals(packageName)) return entry.getValue();
        }
        return null;
    }
}
