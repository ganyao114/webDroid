package com.android.launcher3.model.data;

import com.android.launcher3.LauncherSettings.Favorites;
import com.android.launcher3.util.ContentWriter;
import java.util.List;
import java.util.function.Predicate;

public abstract class CollectionInfo extends ItemInfo {
    public abstract void add(ItemInfo item);
    public abstract List<ItemInfo> getContents();
    public abstract List<WorkspaceItemInfo> getAppContents();

    public boolean anyMatch(Predicate<ItemInfo> predicate) {
        return getContents().stream().anyMatch(predicate);
    }

    @Override
    public void onAddToDatabase(ContentWriter writer) {
        super.onAddToDatabase(writer);
        writer.put(Favorites.TITLE, title);
    }
}
