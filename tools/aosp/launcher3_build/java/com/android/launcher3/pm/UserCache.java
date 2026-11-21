package com.android.launcher3.pm;

import android.content.Context;
import android.os.Process;
import android.os.UserHandle;
import android.os.UserManager;
import com.android.launcher3.util.ContextProvider;
import com.android.launcher3.util.UserIconInfo;

public final class UserCache {
    public static final ContextProvider<UserCache> INSTANCE = UserCache::new;
    private final UserManager users;

    private UserCache(Context context) {
        users = context.getSystemService(UserManager.class);
    }

    public UserIconInfo getUserInfo(UserHandle user) {
        return new UserIconInfo(user, user.equals(Process.myUserHandle()) && users.isManagedProfile());
    }
}
