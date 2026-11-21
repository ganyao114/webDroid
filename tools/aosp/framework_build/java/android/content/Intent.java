package android.content;

import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.util.ArraySet;
import java.util.Set;
import java.net.URISyntaxException;

public class Intent implements Cloneable {
    public static final String ACTION_MAIN = "android.intent.action.MAIN", ACTION_VIEW = "android.intent.action.VIEW";
    public static final String ACTION_SEND = "android.intent.action.SEND";
    public static final String CATEGORY_DEFAULT = "android.intent.category.DEFAULT";
    public static final String CATEGORY_LAUNCHER = "android.intent.category.LAUNCHER";
    public static final String CATEGORY_HOME = "android.intent.category.HOME";
    public static final String CATEGORY_BROWSABLE = "android.intent.category.BROWSABLE";
    public static final int FLAG_ACTIVITY_NEW_TASK = 0x10000000, FLAG_ACTIVITY_RESET_TASK_IF_NEEDED = 0x00200000;
    public static final int FLAG_ACTIVITY_CLEAR_TOP = 0x04000000, FLAG_ACTIVITY_SINGLE_TOP = 0x20000000;

    private String action, pkg, type;
    private android.net.Uri data;
    private ComponentName component;
    private ArraySet<String> categories;
    private Bundle extras;
    private int flags;

    public Intent() {}
    public Intent(String action) { this.action = action; }
    public Intent(String action, android.net.Uri data) { this.action = action; this.data = data; }
    public Intent(Context context, Class<?> cls) { setClass(context, cls); }
    public Intent(Intent other) {
        action = other.action; pkg = other.pkg; data = other.data; type = other.type;
        component = other.component; flags = other.flags;
        if (other.categories != null) for (String category : other.categories) addCategory(category);
        if (other.extras != null) extras = new Bundle(other.extras);
    }
    @Override public Object clone() { return new Intent(this); }
    public static Intent makeMainActivity(ComponentName component) {
        return new Intent(ACTION_MAIN).setComponent(component).addCategory(CATEGORY_LAUNCHER);
    }
    public static Intent makeRestartActivityTask(ComponentName component) {
        return makeMainActivity(component).addFlags(FLAG_ACTIVITY_NEW_TASK | 0x8000);
    }
    public static Intent parseUri(String uri, int flags) throws URISyntaxException {
        final int fragment = uri.lastIndexOf('#');
        if (fragment < 0 || !uri.startsWith("#Intent;", fragment)) return new Intent(ACTION_VIEW, Uri.parse(uri));
        Intent result = new Intent(ACTION_VIEW);
        String scheme = null;
        int start = fragment + "#Intent;".length();
        while (!uri.substring(start).equals("end")) {
            int separator = uri.indexOf(';', start), equals = uri.indexOf('=', start);
            if (separator < 0 || equals < start || equals >= separator)
                throw new URISyntaxException(uri, "Invalid intent fragment", start);
            String key = uri.substring(start, equals);
            String value = Uri.decode(uri.substring(equals + 1, separator));
            switch (key) {
                case "action": result.setAction(value); break;
                case "category": result.addCategory(value); break;
                case "type": result.setType(value); break;
                case "package": result.setPackage(value); break;
                case "component":
                    ComponentName component = ComponentName.unflattenFromString(value);
                    if (component == null) throw new URISyntaxException(uri, "Invalid component", start);
                    result.setComponent(component);
                    break;
                case "scheme": scheme = value; break;
                case "launchFlags":
                    try { result.setFlags(Integer.decode(value)); }
                    catch (NumberFormatException invalid) { throw new URISyntaxException(uri, "Invalid launch flags", start); }
                    break;
                default:
                    if (key.startsWith("S.")) result.putExtra(Uri.decode(key.substring(2)), value);
                    else throw new URISyntaxException(uri, "Unsupported intent property", start);
            }
            start = separator + 1;
        }
        String data = uri.substring(0, fragment);
        if (data.startsWith("intent:")) data = scheme == null ? data.substring(7) : scheme + ':' + data.substring(7);
        if (!data.isEmpty()) result.setDataAndType(Uri.parse(data), result.getType());
        return result;
    }
    public Intent setAction(String action) { this.action = action; return this; }
    public String getAction() { return action; }
    public Intent setPackage(String name) { pkg = name; return this; }
    public String getPackage() { return pkg; }
    public Intent setComponent(ComponentName name) { component = name; return this; }
    public ComponentName getComponent() { return component; }
    public Intent setClassName(String pkg, String name) { return setComponent(new ComponentName(pkg, name)); }
    public Intent setClassName(Context context, String name) { return setComponent(new ComponentName(context, name)); }
    public Intent setClass(Context context, Class<?> type) { return setComponent(new ComponentName(context, type)); }
    public Intent setData(android.net.Uri data) { this.data = data; type = null; return this; }
    public Intent setType(String type) { this.type = type; data = null; return this; }
    public Intent setDataAndType(android.net.Uri data, String type) { this.data = data; this.type = type; return this; }
    public android.net.Uri getData() { return data; }
    public String getDataString() { return data == null ? null : data.toString(); }
    public String getType() { return type; }
    public Intent setFlags(int flags) { this.flags = flags; return this; }
    public Intent addFlags(int flags) { this.flags |= flags; return this; }
    public void removeFlags(int flags) { this.flags &= ~flags; }
    public int getFlags() { return flags; }

    public Intent addCategory(String category) {
        if (categories == null) categories = new ArraySet<>();
        categories.add(category);
        return this;
    }
    public void removeCategory(String category) { if (categories != null) categories.remove(category); }
    public boolean hasCategory(String category) { return categories != null && categories.contains(category); }
    public Set<String> getCategories() { return categories; }
    public String[] categoryArray() {
        if (categories == null) return new String[0];
        String[] result = new String[categories.size()];
        for (int i = 0; i < result.length; ++i) result[i] = categories.valueAt(i);
        return result;
    }

    private Bundle extras() { if (extras == null) extras = new Bundle(); return extras; }
    public Bundle getExtras() { return extras == null ? null : new Bundle(extras); }
    public Intent putExtras(Bundle data) { extras().putAll(data); return this; }
    public Intent replaceExtras(Bundle data) { extras = data == null ? null : new Bundle(data); return this; }
    public Intent putExtra(String key, String value) { extras().putString(key, value); return this; }
    public Intent putExtra(String key, int value) { extras().putInt(key, value); return this; }
    public Intent putExtra(String key, long value) { extras().putLong(key, value); return this; }
    public Intent putExtra(String key, boolean value) { extras().putBoolean(key, value); return this; }
    public Intent putExtra(String key, Bundle value) { extras().putBundle(key, value); return this; }
    public boolean hasExtra(String key) { return extras != null && extras.containsKey(key); }
    public void removeExtra(String key) { if (extras != null) extras.remove(key); }
    public String getStringExtra(String key) { return extras == null ? null : extras.getString(key); }
    public int getIntExtra(String key, int fallback) { return extras == null ? fallback : extras.getInt(key, fallback); }
    public long getLongExtra(String key, long fallback) { return extras == null ? fallback : extras.getLong(key, fallback); }
    public boolean getBooleanExtra(String key, boolean fallback) { return extras == null ? fallback : extras.getBoolean(key, fallback); }
    public Bundle getBundleExtra(String key) { return extras == null ? null : extras.getBundle(key); }

    public ComponentName resolveActivity(PackageManager pm) {
        if (component != null) return component;
        ResolveInfo result = pm.resolveActivity(this, PackageManager.MATCH_DEFAULT_ONLY);
        return result == null ? null : new ComponentName(result.activityInfo.packageName, result.activityInfo.name);
    }
    @Override public String toString() { return "Intent { act=" + action + " pkg=" + pkg + " cmp=" + component + " }"; }
}
