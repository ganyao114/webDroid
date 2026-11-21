package com.android.launcher3.model.data;

import android.content.ComponentName;
import android.content.Intent;
import android.os.UserHandle;
import com.android.launcher3.util.ComponentKey;

public record ResolvedTargetInfo(ComponentName targetActivityComponentName,
                                 ComponentName componentName, UserHandle user) {
    public ComponentKey getTargetComponentKey() {
        ComponentName component = targetActivityComponentName != null
                ? targetActivityComponentName : componentName;
        return component == null ? null : new ComponentKey(component, user);
    }

    public boolean matchTaskLaunchActivity(Intent intent, UserHandle handle) {
        ComponentName component = intent.getComponent();
        return user.equals(handle) && component != null
                && (component.equals(componentName) || component.equals(targetActivityComponentName));
    }
}
