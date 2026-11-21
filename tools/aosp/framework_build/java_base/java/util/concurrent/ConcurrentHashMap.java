package java.util.concurrent;

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

/** Map operations run atomically on the owning interpreter thread. */
public class ConcurrentHashMap<K, V> extends HashMap<K, V> implements ConcurrentMap<K, V> {
    public ConcurrentHashMap() {}
    public ConcurrentHashMap(int capacity) { super(capacity); }
    public ConcurrentHashMap(Map<? extends K, ? extends V> map) { putAll(map); }

    @Override public V get(Object key) { return super.get(Objects.requireNonNull(key)); }
    @Override public boolean containsKey(Object key) { return super.containsKey(Objects.requireNonNull(key)); }
    @Override public boolean containsValue(Object value) { return super.containsValue(Objects.requireNonNull(value)); }
    @Override public V put(K key, V value) { return super.put(Objects.requireNonNull(key), Objects.requireNonNull(value)); }
    @Override public void putAll(Map<? extends K, ? extends V> map) {
        for (Map.Entry<? extends K, ? extends V> entry : map.entrySet()) put(entry.getKey(), entry.getValue());
    }
    @Override public V putIfAbsent(K key, V value) {
        return super.putIfAbsent(Objects.requireNonNull(key), Objects.requireNonNull(value));
    }
    @Override public V remove(Object key) { return super.remove(Objects.requireNonNull(key)); }
    @Override public boolean remove(Object key, Object value) {
        return super.remove(Objects.requireNonNull(key), Objects.requireNonNull(value));
    }
    @Override public boolean replace(K key, V oldValue, V newValue) {
        Objects.requireNonNull(oldValue);
        Objects.requireNonNull(newValue);
        if (!oldValue.equals(get(key))) return false;
        put(key, newValue);
        return true;
    }
    @Override public V replace(K key, V value) {
        Objects.requireNonNull(value);
        return containsKey(key) ? put(key, value) : null;
    }
}
