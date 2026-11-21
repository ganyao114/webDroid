package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.os.Bundle;
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
    @Override public ApplicationInfo getApplicationInfo() { return base.getApplicationInfo(); }
    @Override public Object getSystemService(String name) { return base.getSystemService(name); }
    @Override public String getSystemServiceName(Class<?> type) { return base.getSystemServiceName(type); }
    @Override public Resources.Theme getTheme() { return base.getTheme(); }
    @Override public void setTheme(int id) { base.setTheme(id); }
    @Override public Looper getMainLooper() { return base.getMainLooper(); }
    @Override public Context getApplicationContext() { return base.getApplicationContext(); }
    @Override public void startActivity(Intent intent) { base.startActivity(intent); }
    @Override public void startActivity(Intent intent, Bundle options) { base.startActivity(intent, options); }
}
