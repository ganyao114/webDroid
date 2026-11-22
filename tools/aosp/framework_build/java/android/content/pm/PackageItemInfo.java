package android.content.pm;

import android.graphics.drawable.Drawable;
import android.os.Bundle;

public class PackageItemInfo {
    public String name, packageName;
    public int labelRes, icon, banner, logo;
    public CharSequence nonLocalizedLabel;
    public Bundle metaData;
    public boolean isArchived;

    public PackageItemInfo() {}
    public PackageItemInfo(PackageItemInfo other) {
        name = other.name; packageName = other.packageName; labelRes = other.labelRes; icon = other.icon;
        banner = other.banner; logo = other.logo; nonLocalizedLabel = other.nonLocalizedLabel; metaData = other.metaData;
        isArchived = other.isArchived;
    }
    public CharSequence loadLabel(PackageManager pm) {
        if (nonLocalizedLabel != null) return nonLocalizedLabel;
        CharSequence label = labelRes == 0 ? null : pm.getText(packageName, labelRes, appInfo());
        return label != null ? label : name != null ? name : packageName;
    }
    private ApplicationInfo appInfo() {
        if (this instanceof ApplicationInfo) return (ApplicationInfo) this;
        return this instanceof ComponentInfo ? ((ComponentInfo) this).applicationInfo : null;
    }
    public Drawable loadIcon(PackageManager pm) {
        Drawable drawable = icon == 0 ? null : pm.getDrawable(packageName, icon, appInfo());
        return drawable == null ? pm.getDefaultActivityIcon() : drawable;
    }
}
