package android.util;

import java.util.ArrayList;
import java.util.LinkedHashMap;

public final class ArrayMap<K, V> extends LinkedHashMap<K, V> {
    public K keyAt(int index) {
        return new ArrayList<>(keySet()).get(index);
    }

    public V valueAt(int index) {
        return get(keyAt(index));
    }

    public V setValueAt(int index, V value) {
        return put(keyAt(index), value);
    }

    public void erase() {
        clear();
    }
}
