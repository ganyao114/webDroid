package com.android.launcher3.model;

import com.android.launcher3.model.data.ItemInfo;

@FunctionalInterface
public interface IModelWriter {
    void updateItemInDatabase(ItemInfo item);
}
