package com.android.launcher3.util;

import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.UserHandle;
import android.os.UserManager;
import com.android.launcher3.LauncherSettings.Favorites;
import com.android.launcher3.icons.BitmapInfo;
import java.io.ByteArrayOutputStream;

public final class ContentWriter {
    private final ContentValues values = new ContentValues();
    private final UserManager users;

    public ContentWriter(Context context) {
        users = context.getSystemService(UserManager.class);
    }

    public ContentWriter put(String key, Integer value) { values.put(key, value); return this; }
    public ContentWriter put(String key, String value) { values.put(key, value); return this; }
    public ContentWriter put(String key, CharSequence value) {
        return put(key, value == null ? null : value.toString());
    }
    public ContentWriter put(String key, Intent value) {
        return put(key, value == null ? null : value.toUri(0));
    }
    public ContentWriter put(String key, UserHandle value) {
        values.put(key, users.getSerialNumberForUser(value));
        return this;
    }

    public ContentWriter putIcon(BitmapInfo value, UserHandle user) {
        if (value.icon != null) {
            ByteArrayOutputStream encoded = new ByteArrayOutputStream();
            if (value.icon.compress(Bitmap.CompressFormat.PNG, 100, encoded)) {
                values.put(Favorites.ICON, encoded.toByteArray());
            }
        }
        return this;
    }

    public ContentValues getValues(Context context) {
        return new ContentValues(values);
    }
}
