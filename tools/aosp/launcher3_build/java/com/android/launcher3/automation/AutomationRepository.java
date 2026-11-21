package com.android.launcher3.automation;

import android.os.UserHandle;

/** Automation notifications are optional in the platform build. */
public interface AutomationRepository {
    boolean isPackageAutomated(UserHandle user, String packageName);
}
