package android.util;

import java.util.ArrayList;

public class SparseArray<E> {
    private final ArrayList<Integer> keys;
    private final ArrayList<E> values;

    public SparseArray() { this(10); }

    public SparseArray(int capacity) {
        keys = new ArrayList<>(capacity);
        values = new ArrayList<>(capacity);
    }

    private int locate(int key) {
        int low = 0;
        int high = keys.size();
        while (low < high) {
            int mid = (low + high) >>> 1;
            if (keys.get(mid) < key) low = mid + 1;
            else high = mid;
        }
        return low;
    }

    public E get(int key) { return get(key, null); }

    public E get(int key, E fallback) {
        int i = locate(key);
        return i < size() && keys.get(i) == key ? values.get(i) : fallback;
    }

    public void put(int key, E value) {
        int i = locate(key);
        if (i < size() && keys.get(i) == key) {
            values.set(i, value);
        } else {
            keys.add(i, key);
            values.add(i, value);
        }
    }

    public int size() { return keys.size(); }
    public int keyAt(int index) { return keys.get(index); }
    public E valueAt(int index) { return values.get(index); }

    public void clear() {
        keys.clear();
        values.clear();
    }
}
