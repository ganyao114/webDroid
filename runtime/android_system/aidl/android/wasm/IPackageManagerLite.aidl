package android.wasm;

import android.wasm.PackageRecord;

interface IPackageManagerLite {
    int installApkBytes(in byte[] apkBytes, in @utf8InCpp String apkPath, out PackageRecord outRecord);
    List<PackageRecord> queryLauncherActivities();
    PackageRecord resolveHomeActivity();
    PackageRecord getPackageInfo(in @utf8InCpp String packageName);
}
