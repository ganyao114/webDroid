package android.content.pm;

public class ApplicationInfo extends PackageItemInfo {
    public String sourceDir, publicSourceDir, processName, className, dataDir;
    public int uid, targetSdkVersion = android.os.Build.VERSION.SDK_INT, flags, theme;
    public boolean enabled = true;
    public ApplicationInfo() {}
    public ApplicationInfo(ApplicationInfo other) {
        super(other);
        sourceDir = other.sourceDir; publicSourceDir = other.publicSourceDir; processName = other.processName;
        className = other.className; dataDir = other.dataDir; uid = other.uid; targetSdkVersion = other.targetSdkVersion;
        flags = other.flags; theme = other.theme; enabled = other.enabled;
    }
}
