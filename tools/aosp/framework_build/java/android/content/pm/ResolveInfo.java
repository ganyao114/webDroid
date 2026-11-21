package android.content.pm;

import android.graphics.drawable.Drawable;

public class ResolveInfo {
    public ActivityInfo activityInfo;
    public int labelRes, icon, priority, preferredOrder, match;
    public CharSequence nonLocalizedLabel;
    public boolean isDefault;
    public CharSequence loadLabel(PackageManager pm) {
        return nonLocalizedLabel != null ? nonLocalizedLabel : activityInfo == null ? null : activityInfo.loadLabel(pm);
    }
    public Drawable loadIcon(PackageManager pm) {
        return activityInfo == null ? pm.getDefaultActivityIcon() : activityInfo.loadIcon(pm);
    }
    public int getIconResource() { return icon != 0 ? icon : activityInfo == null ? 0 : activityInfo.getIconResource(); }
}
