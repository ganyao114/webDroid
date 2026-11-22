package android.os;

import java.util.ArrayList;

/** In-process value parcel for framework parcelables. */
public final class Parcel {
    private final ArrayList<Object> values = new ArrayList<>();
    private int position;
    private Parcel() {}
    public static Parcel obtain() { return new Parcel(); }
    public void recycle() { values.clear(); position = 0; }
    public int dataPosition() { return position * 4; }
    public int dataSize() { return values.size() * 4; }
    public void setDataPosition(int position) {
        if (position < 0 || position % 4 != 0 || position > dataSize()) throw new IllegalArgumentException();
        this.position = position / 4;
    }
    private void write(Object value) {
        if (position == values.size()) values.add(value);
        else values.set(position, value);
        position++;
    }
    public void writeInt(int value) { write(value); }
    public void writeFloat(float value) { write(value); }
    public void writeString(String value) { write(value); }
    public void writeBundle(Bundle value) { write(value == null ? null : new Bundle(value)); }
    public void writeBoolean(boolean value) { writeInt(value ? 1 : 0); }
    public void writeCharSequence(CharSequence value) { write(value); }
    public void writeParcelable(Parcelable value, int flags) { write(value); }
    public <T> T readParcelable(ClassLoader loader, Class<T> type) { return type.cast(values.get(position++)); }
    public void writeArrayMap(android.util.ArrayMap<?, ?> value) { write(new android.util.ArrayMap<>(value)); }
    @SuppressWarnings("unchecked")
    public void readArrayMap(android.util.ArrayMap<?, ?> destination, ClassLoader loader) {
        ((android.util.ArrayMap<Object, Object>) destination).putAll(
                (android.util.ArrayMap<Object, Object>) values.get(position++));
    }
    public int readInt() { return (Integer) values.get(position++); }
    public boolean readBoolean() { return readInt() != 0; }
    public Bundle readBundle(ClassLoader loader) {
        Bundle stored = (Bundle) values.get(position++);
        if (stored == null) return null;
        Bundle bundle = new Bundle(stored);
        bundle.setClassLoader(loader);
        return bundle;
    }
    public CharSequence readCharSequence() { return (CharSequence) values.get(position++); }
    public float readFloat() { return (Float) values.get(position++); }
    public void writeString8(String value) { writeString(value); }
    public String readString8() { return readString(); }
    public String readString() { return (String) values.get(position++); }
}
