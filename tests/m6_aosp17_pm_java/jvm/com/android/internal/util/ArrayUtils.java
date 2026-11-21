package com.android.internal.util;

import java.util.function.Predicate;

public final class ArrayUtils {
    public static boolean isEmpty(Object[] array) {
        return array == null || array.length == 0;
    }

    public static <T> T find(T[] array, Predicate<T> predicate) {
        if (array != null) {
            for (T element : array) {
                if (predicate.test(element)) {
                    return element;
                }
            }
        }
        return null;
    }
}
