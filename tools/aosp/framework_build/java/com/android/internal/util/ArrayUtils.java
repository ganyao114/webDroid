package com.android.internal.util;

import java.lang.reflect.Array;

public final class ArrayUtils {
    private ArrayUtils() {}
    public static Object[] newUnpaddedObjectArray(int size) { return new Object[size]; }
    public static int[] newUnpaddedIntArray(int size) { return new int[size]; }
    public static long[] newUnpaddedLongArray(int size) { return new long[size]; }
    public static float[] newUnpaddedFloatArray(int size) { return new float[size]; }
    public static boolean[] newUnpaddedBooleanArray(int size) { return new boolean[size]; }

    @SuppressWarnings("unchecked")
    public static <T> T[] newUnpaddedArray(Class<T> component, int size) {
        return (T[]) Array.newInstance(component, size);
    }

    public static String deepToString(Object value) {
        if (value == null) return "null";
        if (!value.getClass().isArray()) return value.toString();
        StringBuilder result = new StringBuilder("[");
        if (value instanceof Object[]) {
            Object[] items = (Object[]) value;
            for (int i = 0; i < items.length; i++) {
                if (i != 0) result.append(", ");
                result.append(items[i] == value ? "[...]" : deepToString(items[i]));
            }
        } else {
            appendPrimitiveArray(result, value);
        }
        return result.append(']').toString();
    }

    private static void appendPrimitiveArray(StringBuilder result, Object values) {
        int length = primitiveArrayLength(values);
        for (int i = 0; i < length; i++) {
            if (i != 0) result.append(", ");
            if (values instanceof int[]) result.append(((int[]) values)[i]);
            else if (values instanceof long[]) result.append(((long[]) values)[i]);
            else if (values instanceof float[]) result.append(((float[]) values)[i]);
            else if (values instanceof double[]) result.append(((double[]) values)[i]);
            else if (values instanceof char[]) result.append(((char[]) values)[i]);
            else if (values instanceof boolean[]) result.append(((boolean[]) values)[i]);
            else if (values instanceof byte[]) result.append(((byte[]) values)[i]);
            else result.append(((short[]) values)[i]);
        }
    }

    private static int primitiveArrayLength(Object values) {
        if (values instanceof int[]) return ((int[]) values).length;
        if (values instanceof long[]) return ((long[]) values).length;
        if (values instanceof float[]) return ((float[]) values).length;
        if (values instanceof double[]) return ((double[]) values).length;
        if (values instanceof char[]) return ((char[]) values).length;
        if (values instanceof boolean[]) return ((boolean[]) values).length;
        if (values instanceof byte[]) return ((byte[]) values).length;
        return ((short[]) values).length;
    }
}
