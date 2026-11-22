package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

public class ContextWrapper extends Context {
    private Context base;
    public ContextWrapper(Context base) { this.base = base; }
    protected void attachBaseContext(Context base) {
        if (this.base != null) throw new IllegalStateException("Base context already set");
        this.base = base;
    }
    public Context getBaseContext() { return base; }
    @Override public AssetManager getAssets() { return base.getAssets(); }
    @Override public Resources getResources() { return base.getResources(); }
    @Override public PackageManager getPackageManager() { return base.getPackageManager(); }
    @Override public ContentResolver getContentResolver() { return base.getContentResolver(); }
    @Override public String getPackageName() { return base.getPackageName(); }
    @Override public String getOpPackageName() { return base.getOpPackageName(); }
    @Override public int checkSelfPermission(String permission) { return base.checkSelfPermission(permission); }
    @Override public ApplicationInfo getApplicationInfo() { return base.getApplicationInfo(); }
    @Override public Object getSystemService(String name) { return base.getSystemService(name); }
    @Override public String getSystemServiceName(Class<?> type) { return base.getSystemServiceName(type); }
    @Override public Resources.Theme getTheme() { return base.getTheme(); }
    @Override public void setTheme(int id) { base.setTheme(id); }
    @Override public Looper getMainLooper() { return base.getMainLooper(); }
    @Override public Context getApplicationContext() { return base.getApplicationContext(); }
    @Override public SharedPreferences getSharedPreferences(String name, int mode) {
        return base.getSharedPreferences(name, mode);
    }
    @Override public Context createDeviceProtectedStorageContext() { return base.createDeviceProtectedStorageContext(); }
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                             Handler handler, int flags) {
        return base.registerReceiver(receiver, filter, permission, handler, flags);
    }
    @Override public void unregisterReceiver(BroadcastReceiver receiver) { base.unregisterReceiver(receiver); }
    @Override public void sendBroadcast(Intent intent) { base.sendBroadcast(intent); }
    @Override public Context createConfigurationContext(android.content.res.Configuration override) {
        return base.createConfigurationContext(override);
    }
    @Override public void startActivity(Intent intent) { base.startActivity(intent); }
    @Override public void startActivity(Intent intent, Bundle options) { base.startActivity(intent, options); }
}
