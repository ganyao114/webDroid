package android.app;

import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** Package preferences retained for the life of the embedded VM. */
final class MemorySharedPreferences implements SharedPreferences {
    private final Map<String, Object> values = new HashMap<>();
    private final Set<OnSharedPreferenceChangeListener> listeners = new LinkedHashSet<>();
    private final boolean notifyOnClear;

    MemorySharedPreferences(boolean notifyOnClear) { this.notifyOnClear = notifyOnClear; }

    @Override public synchronized Map<String, ?> getAll() {
        Map<String, Object> snapshot = new HashMap<>();
        for (Map.Entry<String, Object> entry : values.entrySet()) {
            Object value = entry.getValue();
            snapshot.put(entry.getKey(), value instanceof Set<?> set ? new LinkedHashSet<>(set) : value);
        }
        return snapshot;
    }

    @Override public synchronized String getString(String key, String fallback) {
        return (String) values.getOrDefault(key, fallback);
    }

    @Override @SuppressWarnings("unchecked")
    public synchronized Set<String> getStringSet(String key, Set<String> fallback) {
        Set<String> stored = (Set<String>) values.get(key);
        return stored == null ? fallback : new LinkedHashSet<>(stored);
    }

    @Override public synchronized int getInt(String key, int fallback) {
        return (Integer) values.getOrDefault(key, fallback);
    }

    @Override public synchronized long getLong(String key, long fallback) {
        return (Long) values.getOrDefault(key, fallback);
    }

    @Override public synchronized float getFloat(String key, float fallback) {
        return (Float) values.getOrDefault(key, fallback);
    }

    @Override public synchronized boolean getBoolean(String key, boolean fallback) {
        return (Boolean) values.getOrDefault(key, fallback);
    }

    @Override public synchronized boolean contains(String key) { return values.containsKey(key); }
    @Override public Editor edit() { return new MemoryEditor(); }

    @Override public synchronized void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {
        listeners.add(Objects.requireNonNull(l));
    }

    @Override public synchronized void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) {
        listeners.remove(l);
    }

    private final class MemoryEditor implements Editor {
        private final Map<String, Object> changes = new LinkedHashMap<>();
        private boolean clear;

        private synchronized Editor put(String key, Object value) {
            changes.put(key, value);
            return this;
        }

        @Override public Editor putString(String key, String value) { return put(key, value); }
        @Override public Editor putStringSet(String key, Set<String> value) {
            return put(key, value == null ? null : new LinkedHashSet<>(value));
        }
        @Override public Editor putInt(String key, int value) { return put(key, value); }
        @Override public Editor putLong(String key, long value) { return put(key, value); }
        @Override public Editor putFloat(String key, float value) { return put(key, value); }
        @Override public Editor putBoolean(String key, boolean value) { return put(key, value); }
        @Override public Editor remove(String key) { return put(key, null); }
        @Override public synchronized Editor clear() { clear = true; return this; }

        @Override public boolean commit() { apply(); return true; }

        @Override public void apply() {
            ArrayList<OnSharedPreferenceChangeListener> callbacks;
            ArrayList<String> updated = new ArrayList<>();
            boolean cleared;
            synchronized (MemorySharedPreferences.this) {
                synchronized (this) {
                    cleared = clear;
                    if (clear) values.clear();
                    for (Map.Entry<String, Object> entry : changes.entrySet()) {
                        String key = entry.getKey();
                        Object value = entry.getValue();
                        Object previous = value == null ? values.remove(key) : values.put(key, value);
                        if (!Objects.equals(previous, value)) updated.add(key);
                    }
                    changes.clear();
                    clear = false;
                }
                if (listeners.isEmpty()) return;
                callbacks = new ArrayList<>(listeners);
            }
            Runnable notify = () -> {
                if (cleared && notifyOnClear) {
                    for (OnSharedPreferenceChangeListener callback : callbacks) {
                        callback.onSharedPreferenceChanged(MemorySharedPreferences.this, null);
                    }
                }
                for (int i = updated.size() - 1; i >= 0; i--) {
                    for (OnSharedPreferenceChangeListener callback : callbacks) {
                        callback.onSharedPreferenceChanged(MemorySharedPreferences.this, updated.get(i));
                    }
                }
            };
            Looper main = Looper.getMainLooper();
            if (main != null && main != Looper.myLooper()) new Handler(main).post(notify);
            else notify.run();
        }
    }
}
