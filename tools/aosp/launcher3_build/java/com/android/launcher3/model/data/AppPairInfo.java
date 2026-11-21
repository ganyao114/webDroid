package com.android.launcher3.model.data;

import com.android.launcher3.LauncherSettings.Favorites;
import java.util.ArrayList;
import java.util.List;

public final class AppPairInfo extends CollectionInfo {
    private final ArrayList<WorkspaceItemInfo> apps = new ArrayList<>(2);

    public AppPairInfo() {
        itemType = Favorites.ITEM_TYPE_APP_GROUP;
    }

    @Override
    public void add(ItemInfo item) {
        if (!(item instanceof WorkspaceItemInfo app) || apps.size() == 2) {
            throw new IllegalArgumentException("Expected two workspace apps");
        }
        apps.add(app);
    }

    @Override public List<ItemInfo> getContents() { return new ArrayList<>(apps); }
    @Override public List<WorkspaceItemInfo> getAppContents() { return new ArrayList<>(apps); }
}
