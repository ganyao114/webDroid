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
    public int readInt() { return (Integer) values.get(position++); }
    public float readFloat() { return (Float) values.get(position++); }
    public void writeString8(String value) { writeString(value); }
    public String readString8() { return readString(); }
    public String readString() { return (String) values.get(position++); }
}
