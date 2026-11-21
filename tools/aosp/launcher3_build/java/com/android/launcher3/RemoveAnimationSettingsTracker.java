package com.android.launcher3;

import android.content.Context;
import android.net.Uri;
import android.provider.Settings;
import com.android.launcher3.util.ContextProvider;

public final class RemoveAnimationSettingsTracker {
    public static final Uri WINDOW_ANIMATION_SCALE_URI =
            Settings.Global.getUriFor(Settings.Global.WINDOW_ANIMATION_SCALE);
    public static final ContextProvider<RemoveAnimationSettingsTracker> INSTANCE =
            RemoveAnimationSettingsTracker::new;
    private final Context context;

    private RemoveAnimationSettingsTracker(Context context) { this.context = context; }

    public float getValue(Uri setting) {
        return Settings.Global.getFloat(context.getContentResolver(), setting.getLastPathSegment(), 1);
    }
}
