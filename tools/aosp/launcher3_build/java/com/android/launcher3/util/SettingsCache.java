package com.android.launcher3.util;

import android.content.Context;
import android.net.Uri;
import android.provider.Settings;

public final class SettingsCache {
    public static final ContextProvider<SettingsCache> INSTANCE = SettingsCache::new;
    private final Context context;

    private SettingsCache(Context context) {
        this.context = context;
    }

    public boolean getValue(Uri uri) {
        return Settings.Secure.getInt(context.getContentResolver(), uri.getLastPathSegment(), 0) != 0;
    }
}
