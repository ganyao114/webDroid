package android.appwidget;

import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.RemoteViews;
import java.util.concurrent.Executor;

public class AppWidgetHostView extends FrameLayout {
    private int id;
    private AppWidgetProviderInfo info;
    private boolean displayingFallback;
    public AppWidgetHostView(Context context) { super(context); }
    /** The empty host has no remote widget inflation to schedule. */
    public void setExecutor(Executor executor) {}
    public void setOnLightBackground(boolean onLightBackground) {}
    public int getAppWidgetId() { return id; }
    public AppWidgetProviderInfo getAppWidgetInfo() { return info; }
    public void setAppWidget(int id, AppWidgetProviderInfo info) {
        if (this.info != info || this.id != id) displayingFallback = false;
        this.id = id;
        this.info = info;
    }
    protected View getErrorView() { return new View(getContext()); }
    public void updateAppWidget(RemoteViews views) {
        View content = null;
        if (views != null && views.getLayoutId() != 0) {
            try { content = views.apply(getContext(), this); }
            catch (RuntimeException unavailable) { android.util.Log.w("AppWidgetHostView", "Widget layout unavailable", unavailable); }
        }
        if (content == null && displayingFallback) return;
        displayingFallback = content == null;
        if (displayingFallback) content = getErrorView();
        removeAllViews();
        addView(content, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
    }
}
