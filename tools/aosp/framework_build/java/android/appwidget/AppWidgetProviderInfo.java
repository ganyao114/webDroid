package android.appwidget;

import android.content.ComponentName;
import android.os.UserHandle;

public class AppWidgetProviderInfo {
    public ComponentName provider;
    public int autoAdvanceViewId = -1;
    public UserHandle getProfile() { return UserHandle.SYSTEM; }
}
