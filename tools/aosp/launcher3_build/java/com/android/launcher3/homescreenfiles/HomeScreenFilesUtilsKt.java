package com.android.launcher3.homescreenfiles;

import com.android.launcher3.LauncherSettings.Favorites;
import com.android.launcher3.model.data.ItemInfo;

public final class HomeScreenFilesUtilsKt {
    private HomeScreenFilesUtilsKt() {}

    public static boolean isFileSystemFileItem(ItemInfo item) {
        return item.itemType == Favorites.ITEM_TYPE_FILE_SYSTEM_FILE;
    }
}
