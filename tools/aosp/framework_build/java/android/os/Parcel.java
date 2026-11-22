package android.os;

import java.util.ArrayList;
import java.util.List;

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
    public void writeLong(long value) { write(value); }
    public void writeFloat(float value) { write(value); }
    public void writeIntArray(int[] value) { write(value == null ? null : value.clone()); }
    public void writeFloatArray(float[] value) { write(value == null ? null : value.clone()); }
    public void writeString(String value) { write(value); }
    public void writeBundle(Bundle value) { write(value == null ? null : new Bundle(value)); }
    public void writeBoolean(boolean value) { writeInt(value ? 1 : 0); }
    public void writeStrongBinder(IBinder value) { write(value); }
    public IBinder readStrongBinder() { return (IBinder) values.get(position++); }
    public void writeCharSequence(CharSequence value) { write(value); }
    public void writeList(List<?> value) { write(value == null ? null : new ArrayList<>(value)); }
    public <T> void readList(List<? super T> target, ClassLoader loader, Class<T> type) {
        List<?> stored = (List<?>) values.get(position++);
        if (stored != null) for (Object value : stored) target.add(type.cast(value));
    }
    public void writeParcelable(Parcelable value, int flags) { write(value); }
    public <T> T readParcelable(ClassLoader loader, Class<T> type) { return type.cast(values.get(position++)); }
    public <T extends Parcelable> void writeTypedObject(T value, int flags) {
        writeInt(value == null ? 0 : 1);
        if (value != null) value.writeToParcel(this, flags);
    }
    public <T> T readTypedObject(Parcelable.Creator<T> creator) {
        return readInt() == 0 ? null : creator.createFromParcel(this);
    }
    public <T extends Parcelable> void writeTypedArray(T[] array, int flags) {
        writeInt(array == null ? -1 : array.length);
        if (array != null) for (T value : array) writeTypedObject(value, flags);
    }
    public <T> T[] createTypedArray(Parcelable.Creator<T> creator) {
        int size = readInt();
        if (size < 0) return null;
        T[] array = creator.newArray(size);
        for (int i = 0; i < size; i++) array[i] = readTypedObject(creator);
        return array;
    }
    public void writeArrayMap(android.util.ArrayMap<?, ?> value) { write(new android.util.ArrayMap<>(value)); }
    @SuppressWarnings("unchecked")
    public void readArrayMap(android.util.ArrayMap<?, ?> destination, ClassLoader loader) {
        ((android.util.ArrayMap<Object, Object>) destination).putAll(
                (android.util.ArrayMap<Object, Object>) values.get(position++));
    }
    public int readInt() { return (Integer) values.get(position++); }
    public long readLong() { return (Long) values.get(position++); }
    public int[] createIntArray() {
        int[] array = (int[]) values.get(position++);
        return array == null ? null : array.clone();
    }
    public float[] createFloatArray() {
        float[] array = (float[]) values.get(position++);
        return array == null ? null : array.clone();
    }
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
