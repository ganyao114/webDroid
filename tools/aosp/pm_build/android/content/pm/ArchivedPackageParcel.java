// Compile-time declaration for a hidden AOSP API.
package android.content.pm;

// Fields read by ApkLite, declared in the upstream ArchivedPackageParcel.aidl.
public final class ArchivedPackageParcel {
    public String packageName;
    public SigningDetails signingDetails;
    public int versionCode;
    public int versionCodeMajor;
    public int targetSdkVersion;
}
