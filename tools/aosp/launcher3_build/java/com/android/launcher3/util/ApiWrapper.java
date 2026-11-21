package com.android.launcher3.util;

import android.app.Person;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.net.Uri;
import android.os.UserHandle;

public final class ApiWrapper {
    private static final ApiWrapper SDK = new ApiWrapper();
    public static final ContextProvider<ApiWrapper> INSTANCE = context -> SDK;

    public Intent getAppMarketActivityIntent(String packageName, UserHandle user) {
        return new Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=" + packageName))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }

    public Person[] getPersons(ShortcutInfo shortcut) {
        return new Person[0];
    }
}
