package android.content.pm;

import android.graphics.drawable.Drawable;

public class ComponentInfo extends PackageItemInfo {
    public ApplicationInfo applicationInfo;
    public String processName;
    public boolean enabled = true, exported;
    public boolean isEnabled() { return enabled && (applicationInfo == null || applicationInfo.enabled); }
    public int getIconResource() { return icon != 0 ? icon : applicationInfo == null ? 0 : applicationInfo.icon; }
    @Override public CharSequence loadLabel(PackageManager pm) {
        if (nonLocalizedLabel != null || labelRes != 0 || applicationInfo == null) return super.loadLabel(pm);
        return applicationInfo.loadLabel(pm);
    }
    @Override public Drawable loadIcon(PackageManager pm) {
        return icon == 0 && applicationInfo != null ? applicationInfo.loadIcon(pm) : super.loadIcon(pm);
    }
}
