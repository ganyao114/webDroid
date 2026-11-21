package android.graphics;

import android.os.Parcel;
import android.os.Parcelable;

public final class Rect implements Parcelable {
    public int left, top, right, bottom;
    public Rect() {}
    public Rect(int left, int top, int right, int bottom) { set(left, top, right, bottom); }
    public Rect(Rect other) { if (other != null) set(other); }
    public void set(int left, int top, int right, int bottom) {
        this.left = left;
        this.top = top;
        this.right = right;
        this.bottom = bottom;
    }
    public void set(Rect other) { set(other.left, other.top, other.right, other.bottom); }
    public void setEmpty() { set(0, 0, 0, 0); }
    public boolean isEmpty() { return left >= right || top >= bottom; }
    public int width() { return right - left; }
    public int height() { return bottom - top; }
    public int centerX() { return (left + right) >> 1; }
    public int centerY() { return (top + bottom) >> 1; }
    public float exactCenterX() { return (left + right) * 0.5f; }
    public float exactCenterY() { return (top + bottom) * 0.5f; }
    public void offset(int dx, int dy) { set(left + dx, top + dy, right + dx, bottom + dy); }
    public void offsetTo(int left, int top) { offset(left - this.left, top - this.top); }
    public void inset(int dx, int dy) { inset(dx, dy, dx, dy); }
    public void inset(int l, int t, int r, int b) { set(left + l, top + t, right - r, bottom - b); }
    public void inset(Rect insets) { inset(insets.left, insets.top, insets.right, insets.bottom); }
    public boolean contains(int x, int y) { return left <= x && x < right && top <= y && y < bottom; }
    public boolean contains(int l, int t, int r, int b) {
        return !isEmpty() && left <= l && top <= t && right >= r && bottom >= b;
    }
    public boolean contains(Rect other) { return contains(other.left, other.top, other.right, other.bottom); }
    public boolean intersects(int l, int t, int r, int b) { return left < r && l < right && top < b && t < bottom; }
    public boolean intersect(int l, int t, int r, int b) {
        if (!intersects(l, t, r, b)) return false;
        setUncheckedIntersection(l, t, r, b);
        return true;
    }
    private void setUncheckedIntersection(int l, int t, int r, int b) {
        set(Math.max(left, l), Math.max(top, t), Math.min(right, r), Math.min(bottom, b));
    }
    public boolean intersect(Rect other) { return intersect(other.left, other.top, other.right, other.bottom); }
    public boolean setIntersect(Rect a, Rect b) {
        if (!intersects(a, b)) return false;
        set(Math.max(a.left, b.left), Math.max(a.top, b.top),
                Math.min(a.right, b.right), Math.min(a.bottom, b.bottom));
        return true;
    }
    public static boolean intersects(Rect a, Rect b) { return a.intersects(b.left, b.top, b.right, b.bottom); }
    public void union(int l, int t, int r, int b) {
        if (l >= r || t >= b) return;
        if (isEmpty()) set(l, t, r, b);
        else set(Math.min(left, l), Math.min(top, t), Math.max(right, r), Math.max(bottom, b));
    }
    public void union(Rect other) { union(other.left, other.top, other.right, other.bottom); }
    public void union(int x, int y) {
        set(Math.min(left, x), Math.min(top, y), Math.max(right, x), Math.max(bottom, y));
    }
    public void sort() { set(Math.min(left, right), Math.min(top, bottom),
            Math.max(left, right), Math.max(top, bottom)); }
    @Override public boolean equals(Object other) {
        if (!(other instanceof Rect)) return false;
        Rect rect = (Rect) other;
        return left == rect.left && top == rect.top && right == rect.right && bottom == rect.bottom;
    }
    @Override public int hashCode() { return ((left * 31 + top) * 31 + right) * 31 + bottom; }
    @Override public String toString() { return "Rect(" + left + ", " + top + " - " + right + ", " + bottom + ")"; }
    public String toShortString() { return "[" + left + "," + top + "][" + right + "," + bottom + "]"; }
    public String flattenToString() { return left + " " + top + " " + right + " " + bottom; }
    public static Rect unflattenFromString(String text) {
        if (text == null) return null;
        String[] parts = text.split(" ");
        if (parts.length != 4) return null;
        try {
            return new Rect(Integer.parseInt(parts[0]), Integer.parseInt(parts[1]),
                    Integer.parseInt(parts[2]), Integer.parseInt(parts[3]));
        } catch (NumberFormatException error) {
            return null;
        }
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) {
        parcel.writeInt(left); parcel.writeInt(top); parcel.writeInt(right); parcel.writeInt(bottom);
    }
    public void readFromParcel(Parcel parcel) {
        set(parcel.readInt(), parcel.readInt(), parcel.readInt(), parcel.readInt());
    }
    public static final Parcelable.Creator<Rect> CREATOR = new Parcelable.Creator<Rect>() {
        public Rect createFromParcel(Parcel parcel) { Rect result = new Rect(); result.readFromParcel(parcel); return result; }
        public Rect[] newArray(int size) { return new Rect[size]; }
    };
}
