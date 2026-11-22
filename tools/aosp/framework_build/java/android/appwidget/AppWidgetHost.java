package android.appwidget;

import android.content.Context;

/** An empty widget host when no providers are connected. */
public class AppWidgetHost {
    public AppWidgetHost(Context context, int hostId) {}
    public void startListening() {}
    public void stopListening() {}
    public void deleteHost() {}
    public void deleteAppWidgetId(int id) {}
    public int[] getAppWidgetIds() { return new int[0]; }
    protected void clearViews() {}
    protected void onProvidersChanged() {}
    protected void onAppWidgetRemoved(int id) {}
}
