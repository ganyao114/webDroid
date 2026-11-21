package java.lang;

import java.io.Serializable;

/** Enum support for the framework and application DEX files in the embedded VM. */
public abstract class Enum<E extends Enum<E>> implements Comparable<E>, Serializable {
    private final String name;
    private final int ordinal;

    protected Enum(String name, int ordinal) {
        this.name = name;
        this.ordinal = ordinal;
    }

    public final String name() { return name; }
    public final int ordinal() { return ordinal; }
    @Override public String toString() { return name; }
    @Override public final boolean equals(Object other) { return this == other; }
    @Override public final int hashCode() { return System.identityHashCode(this); }

    @Override public final int compareTo(E other) {
        if (getDeclaringClass() != other.getDeclaringClass()) throw new ClassCastException();
        return ordinal - other.ordinal();
    }

    @SuppressWarnings("unchecked")
    public final Class<E> getDeclaringClass() { return (Class<E>) declaringClass(getClass()); }

    @SuppressWarnings("unchecked")
    public static <T extends Enum<T>> T valueOf(Class<T> enumClass, String name) {
        if (enumClass == null || name == null) throw new NullPointerException();
        return (T) constant(enumClass, name);
    }

    private static native Class<?> declaringClass(Class<?> type);
    private static native Enum<?> constant(Class<?> type, String name);
}
