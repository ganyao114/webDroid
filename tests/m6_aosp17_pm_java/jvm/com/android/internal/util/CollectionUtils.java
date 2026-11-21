package com.android.internal.util;

import java.util.Collection;
import java.util.Map;

public final class CollectionUtils {
    public static boolean isEmpty(Collection<?> collection) {
        return collection == null || collection.isEmpty();
    }

    public static int size(Map<?, ?> map) {
        return map == null ? 0 : map.size();
    }
}
