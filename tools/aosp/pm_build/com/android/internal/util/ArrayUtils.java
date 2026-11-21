// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import java.util.Map;
import java.util.function.Predicate;

public final class ArrayUtils {
    public static native boolean isEmpty(Object[] array);
    public static native int size(Map<?, ?> map);
    public static native <T> boolean contains(T[] array, T element);
    public static native <T> T find(T[] array, Predicate<T> predicate);
    public static native <T> T[] appendElement(Class<T> type, T[] array, T element);
    public static native <T> T[] appendElement(Class<T> type, T[] array, T element,
            boolean allowDuplicates);
    public static native long[] appendLong(long[] array, long value, boolean allowDuplicates);
}
