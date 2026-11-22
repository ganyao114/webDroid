package org.wasmandroid.ui;

import java.util.List;

/** Java API entry points used by the offline AndroidX preference widget on the embedded VM. */
public final class PreferenceRuntime {
    private PreferenceRuntime() {}

    public static String packageName(Class<?> type) {
        String name = type.getName();
        int delimiter = name.lastIndexOf('.');
        return delimiter < 0 ? "" : name.substring(0, delimiter);
    }

    public static <T> int binarySearch(List<? extends Comparable<? super T>> list, T key) {
        int low = 0;
        int high = list.size() - 1;
        while (low <= high) {
            int middle = (low + high) >>> 1;
            int order = list.get(middle).compareTo(key);
            if (order < 0) {
                low = middle + 1;
            } else if (order > 0) {
                high = middle - 1;
            } else {
                return middle;
            }
        }
        return -(low + 1);
    }
}
