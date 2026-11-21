package com.android.launcher3.model.data;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import com.android.launcher3.LauncherSettings.Favorites;

/** Public-SDK adapter for the upstream model's archived-app and App Lock dependencies. */
public class AppInfo extends ItemInfoWithIcon implements WorkspaceItemFactory {
    public Intent intent;
    public ComponentName componentName;
    public int uid = -1;

    public AppInfo() {
        itemType = Favorites.ITEM_TYPE_APPLICATION;
        container = Favorites.CONTAINER_ALL_APPS;
    }

    private AppInfo(AppInfo source) {
        super(source);
        intent = source.intent == null ? null : new Intent(source.intent);
        componentName = source.componentName;
        uid = source.uid;
    }

    public static Intent makeLaunchIntent(ComponentName component) {
        return new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .setComponent(component)
                .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
    }

    @Override
    public Intent getIntent() { return intent; }

    @Override
    public ComponentName getTargetComponent() { return componentName; }

    @Override
    public WorkspaceItemInfo makeWorkspaceItem(Context context) {
        return new WorkspaceItemInfo(this);
    }

    @Override
    public AppInfo clone() { return new AppInfo(this); }
}
