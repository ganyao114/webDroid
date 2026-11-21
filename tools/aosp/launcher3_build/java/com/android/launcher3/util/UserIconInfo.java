package com.android.launcher3.util;

import android.os.Process;
import android.os.UserHandle;

public final class UserIconInfo {
    public final UserHandle user;
    private final boolean managed;

    public UserIconInfo(UserHandle user, boolean managed) {
        this.user = user;
        this.managed = managed;
    }

    public boolean isMain() { return !managed && user.equals(Process.myUserHandle()); }
    public boolean isWork() { return managed; }
    // The SDK build enumerates only the current user and does not expose other profile types.
    public boolean isPrivate() { return false; }
    public boolean isCloned() { return false; }
}
