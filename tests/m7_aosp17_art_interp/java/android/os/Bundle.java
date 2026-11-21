package android.os;

import java.util.LinkedHashMap;
import java.util.Set;

public final class Bundle implements Cloneable {
    private final LinkedHashMap<String, Object> values = new LinkedHashMap<>();

    public Bundle() {}

    public void putString(String key, String value) { values.put(key, value); }
    public void putInt(String key, int value) { values.put(key, value); }
    public void putBoolean(String key, boolean value) { values.put(key, value); }
    public Object get(String key) { return values.get(key); }
    public String getString(String key) { return get(key) instanceof String value ? value : null; }
    public int getInt(String key) { return getInt(key, 0); }
    public int getInt(String key, int fallback) {
        return get(key) instanceof Integer value ? value : fallback;
    }
    public boolean getBoolean(String key) { return getBoolean(key, false); }
    public boolean getBoolean(String key, boolean fallback) {
        return get(key) instanceof Boolean value ? value : fallback;
    }
    public boolean containsKey(String key) { return values.containsKey(key); }
    public Set<String> keySet() { return values.keySet(); }
    public int size() { return values.size(); }
}
