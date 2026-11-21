package android.graphics;

import android.os.Parcel;
import android.os.Parcelable;
import org.wasmandroid.ui.NativeMath;

public class RectF implements Parcelable {
    public float left, top, right, bottom;
    public RectF() {}
    public RectF(float left, float top, float right, float bottom) { set(left, top, right, bottom); }
    public RectF(RectF other) { if (other != null) set(other); }
    public RectF(Rect other) { if (other != null) set(other); }
    public void set(float left, float top, float right, float bottom) {
        this.left = left;
        this.top = top;
        this.right = right;
        this.bottom = bottom;
    }
    public void set(RectF other) { set(other.left, other.top, other.right, other.bottom); }
    public void set(Rect other) { set(other.left, other.top, other.right, other.bottom); }
    public void setEmpty() { set(0, 0, 0, 0); }
    public boolean isEmpty() { return left >= right || top >= bottom; }
    public float width() { return right - left; }
    public float height() { return bottom - top; }
    public float centerX() { return (left + right) * 0.5f; }
    public float centerY() { return (top + bottom) * 0.5f; }
    public void offset(float dx, float dy) { set(left + dx, top + dy, right + dx, bottom + dy); }
    public void offsetTo(float left, float top) { offset(left - this.left, top - this.top); }
    public void inset(float dx, float dy) { set(left + dx, top + dy, right - dx, bottom - dy); }
    public boolean contains(float x, float y) { return left <= x && x < right && top <= y && y < bottom; }
    public boolean contains(float l, float t, float r, float b) {
        return !isEmpty() && left <= l && top <= t && right >= r && bottom >= b;
    }
    public boolean contains(RectF other) { return contains(other.left, other.top, other.right, other.bottom); }
    public boolean intersects(float l, float t, float r, float b) {
        return left < r && l < right && top < b && t < bottom;
    }
    public boolean intersect(float l, float t, float r, float b) {
        if (!intersects(l, t, r, b)) return false;
        set(Math.max(left, l), Math.max(top, t), Math.min(right, r), Math.min(bottom, b));
        return true;
    }
    public boolean intersect(RectF other) { return intersect(other.left, other.top, other.right, other.bottom); }
    public boolean setIntersect(RectF a, RectF b) {
        if (!intersects(a, b)) return false;
        set(Math.max(a.left, b.left), Math.max(a.top, b.top),
                Math.min(a.right, b.right), Math.min(a.bottom, b.bottom));
        return true;
    }
    public static boolean intersects(RectF a, RectF b) { return a.intersects(b.left, b.top, b.right, b.bottom); }
    public void union(float l, float t, float r, float b) {
        if (l >= r || t >= b) return;
        if (isEmpty()) set(l, t, r, b);
        else set(Math.min(left, l), Math.min(top, t), Math.max(right, r), Math.max(bottom, b));
    }
    public void union(RectF other) { union(other.left, other.top, other.right, other.bottom); }
    public void union(float x, float y) {
        set(Math.min(left, x), Math.min(top, y), Math.max(right, x), Math.max(bottom, y));
    }
    public void sort() { set(Math.min(left, right), Math.min(top, bottom),
            Math.max(left, right), Math.max(top, bottom)); }
    public void round(Rect out) { out.set(NativeMath.round(left), NativeMath.round(top),
            NativeMath.round(right), NativeMath.round(bottom)); }
    public void roundOut(Rect out) { out.set(floor(left), floor(top), -floor(-right), -floor(-bottom)); }
    private static int floor(float value) { int intValue = (int) value; return value < intValue ? intValue - 1 : intValue; }
    @Override public boolean equals(Object other) {
        if (!(other instanceof RectF)) return false;
        RectF rect = (RectF) other;
        return left == rect.left && top == rect.top && right == rect.right && bottom == rect.bottom;
    }
    @Override public int hashCode() { return ((bits(left) * 31 + bits(top)) * 31 + bits(right)) * 31 + bits(bottom); }
    private static int bits(float value) { return value == 0 ? 0 : Float.floatToRawIntBits(value); }
    @Override public String toString() { return "RectF(" + left + ", " + top + ", " + right + ", " + bottom + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) {
        parcel.writeFloat(left); parcel.writeFloat(top); parcel.writeFloat(right); parcel.writeFloat(bottom);
    }
    public void readFromParcel(Parcel parcel) {
        set(parcel.readFloat(), parcel.readFloat(), parcel.readFloat(), parcel.readFloat());
    }
    public static final Parcelable.Creator<RectF> CREATOR = new Parcelable.Creator<RectF>() {
        public RectF createFromParcel(Parcel parcel) { RectF result = new RectF(); result.readFromParcel(parcel); return result; }
        public RectF[] newArray(int size) { return new RectF[size]; }
    };
}
