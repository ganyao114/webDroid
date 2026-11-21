package android.util;

public abstract class Property<T, V> {
    private final Class<V> type;
    private final String name;
    public Property(Class<V> type, String name) { this.type = type; this.name = name; }
    public String getName() { return name; }
    public Class<V> getType() { return type; }
    public abstract V get(T target);
    public void set(T target, V value) { throw new UnsupportedOperationException("Read-only property " + name); }
}
