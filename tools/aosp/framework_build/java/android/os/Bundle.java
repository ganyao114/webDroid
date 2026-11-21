package android.os;

import android.util.ArrayMap;
import java.util.Set;

public final class Bundle implements Cloneable {
    private final ArrayMap<String, Object> values = new ArrayMap<>();
    public Bundle() {}
    public Bundle(Bundle other) { putAll(other); }
    public void putAll(Bundle other) {
        if (other != null) for (int i = 0; i < other.values.size(); ++i) {
            values.put(other.values.keyAt(i), other.values.valueAt(i));
        }
    }
    public boolean containsKey(String key) { return values.containsKey(key); }
    public Object get(String key) { return values.get(key); }
    public void remove(String key) { values.remove(key); }
    public void clear() { values.clear(); }
    public boolean isEmpty() { return values.isEmpty(); }
    public int size() { return values.size(); }
    public Set<String> keySet() { return values.keySet(); }
    public void putString(String key, String value) { values.put(key, value); }
    public String getString(String key) { return getString(key, null); }
    public String getString(String key, String fallback) { Object v = get(key); return v instanceof String ? (String) v : fallback; }
    public void putInt(String key, int value) { values.put(key, value); }
    public int getInt(String key) { return getInt(key, 0); }
    public int getInt(String key, int fallback) { Object v = get(key); return v instanceof Integer ? (Integer) v : fallback; }
    public void putLong(String key, long value) { values.put(key, value); }
    public long getLong(String key) { return getLong(key, 0); }
    public long getLong(String key, long fallback) { Object v = get(key); return v instanceof Long ? (Long) v : fallback; }
    public void putBoolean(String key, boolean value) { values.put(key, value); }
    public boolean getBoolean(String key) { return getBoolean(key, false); }
    public boolean getBoolean(String key, boolean fallback) { Object v = get(key); return v instanceof Boolean ? (Boolean) v : fallback; }
    public void putFloat(String key, float value) { values.put(key, value); }
    public float getFloat(String key) { return getFloat(key, 0); }
    public float getFloat(String key, float fallback) { Object v = get(key); return v instanceof Float ? (Float) v : fallback; }
    public void putCharSequence(String key, CharSequence value) { values.put(key, value); }
    public CharSequence getCharSequence(String key) { Object v = get(key); return v instanceof CharSequence ? (CharSequence) v : null; }
    public void putBundle(String key, Bundle value) { values.put(key, value); }
    public Bundle getBundle(String key) { Object v = get(key); return v instanceof Bundle ? (Bundle) v : null; }
    public void putParcelable(String key, Parcelable value) { values.put(key, value); }
    @SuppressWarnings("unchecked") public <T extends Parcelable> T getParcelable(String key) {
        Object v = get(key); return v instanceof Parcelable ? (T) v : null;
    }
    @Override public Object clone() { return new Bundle(this); }
}
