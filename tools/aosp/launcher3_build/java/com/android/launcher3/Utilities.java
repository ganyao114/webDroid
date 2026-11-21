package com.android.launcher3;

import android.app.ActivityManager;
import android.content.Context;
import android.content.res.Resources;
import android.view.View;

public final class Utilities {
    public static final String[] EMPTY_STRING_ARRAY = new String[0];

    private Utilities() {}

    public static String trim(CharSequence text) {
        return text == null ? "" : text.toString().trim();
    }

    public static boolean isRtl(Resources resources) {
        return resources.getConfiguration().getLayoutDirection() == View.LAYOUT_DIRECTION_RTL;
    }

    public static boolean shouldReduceWorkspaceBlurUsage(Context context) {
        return Flags.reduceWorkspaceBlurUsage()
                && context.getResources().getBoolean(R.bool.reduce_workspace_blur_usage);
    }

    public static int boundToRange(int value, int lower, int upper) {
        return Math.max(lower, Math.min(value, upper));
    }

    public static boolean isRunningInTestHarness() {
        return ActivityManager.isRunningInUserTestHarness();
    }

    public static boolean shouldEnableMouseInteractionChanges(Context context) {
        return Flags.enableMouseInteractionChanges()
                && context.getResources().getBoolean(R.bool.desktop_form_factor);
    }
}
