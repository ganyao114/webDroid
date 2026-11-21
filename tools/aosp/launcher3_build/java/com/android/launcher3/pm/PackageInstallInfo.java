package com.android.launcher3.pm;

import android.os.UserHandle;

public final class PackageInstallInfo {
    public static final int STATUS_INSTALLED = 0;
    public static final int STATUS_INSTALLING = 1;
    public static final int STATUS_INSTALLED_DOWNLOADING = 2;
    public static final int STATUS_FAILED = 3;

    public final String packageName;
    public final UserHandle user;
    public final int state;
    public final int progress;

    public PackageInstallInfo(String packageName, int state, int progress, UserHandle user) {
        this.packageName = packageName;
        this.state = state;
        this.progress = progress;
        this.user = user;
    }
}
