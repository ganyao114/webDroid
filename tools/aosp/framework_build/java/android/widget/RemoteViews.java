package android.widget;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import java.util.Objects;

/** Layout reference used by the embedded widget host. */
public class RemoteViews {
    @java.lang.annotation.Target(java.lang.annotation.ElementType.TYPE)
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
    public @interface RemoteView {}

    private final String packageName;
    private final int layoutId;

    public RemoteViews(String packageName, int layoutId) {
        this.packageName = Objects.requireNonNull(packageName);
        this.layoutId = layoutId;
    }

    public String getPackage() { return packageName; }
    public int getLayoutId() { return layoutId; }
    public View apply(Context context, ViewGroup parent) {
        if (!context.getPackageName().equals(packageName)) {
            throw new IllegalArgumentException("Remote widget APK is unavailable: " + packageName);
        }
        return LayoutInflater.from(context).inflate(layoutId, parent, false);
    }
}
