package android.appwidget;

import android.content.ComponentName;
import android.content.Context;
import android.os.Bundle;
import android.os.UserHandle;
import java.util.Collections;
import java.util.List;

/** Widget service connection for a device with no registered widget providers. */
public final class AppWidgetManager {
    public static final int INVALID_APPWIDGET_ID = 0;
    public static AppWidgetManager getInstance(Context context) {
        return context.getSystemService(AppWidgetManager.class);
    }
    public List<AppWidgetProviderInfo> getInstalledProviders() { return Collections.emptyList(); }
    public List<AppWidgetProviderInfo> getInstalledProvidersForProfile(UserHandle profile) {
        return getInstalledProviders();
    }
    public List<AppWidgetProviderInfo> getInstalledProvidersForPackage(String packageName, UserHandle profile) {
        return getInstalledProviders();
    }
    public AppWidgetProviderInfo getAppWidgetInfo(int id) { return null; }
    public Bundle getAppWidgetOptions(int id) { return new Bundle(); }
    public boolean bindAppWidgetIdIfAllowed(int id, UserHandle profile, ComponentName provider, Bundle options) {
        return false;
    }
}
