package com.android.launcher3.views;

import android.content.Context;
import android.content.ContextWrapper;
import com.android.launcher3.DeviceProfile;
import com.android.launcher3.dragndrop.DragLayer;

public interface ActivityContext {
    DeviceProfile getDeviceProfile();
    DragLayer getDragLayer();

    static ActivityContext lookupContext(Context context) {
        while (context instanceof ContextWrapper wrapper) {
            if (context instanceof ActivityContext activity) return activity;
            context = wrapper.getBaseContext();
        }
        return context instanceof ActivityContext activity ? activity : null;
    }
}
