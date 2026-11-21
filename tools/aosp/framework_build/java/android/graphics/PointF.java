package android.graphics;

import android.os.Parcel;
import android.os.Parcelable;
import org.wasmandroid.ui.NativeMath;

public class PointF implements Parcelable {
    public float x, y;
    public PointF() {}
    public PointF(float x, float y) { set(x, y); }
    public PointF(Point other) { set(other.x, other.y); }
    public PointF(PointF other) { set(other.x, other.y); }
    public final void set(float x, float y) { this.x = x; this.y = y; }
    public final void set(PointF other) { set(other.x, other.y); }
    public final void negate() { set(-x, -y); }
    public final void offset(float x, float y) { set(this.x + x, this.y + y); }
    public final boolean equals(float x, float y) { return this.x == x && this.y == y; }
    @Override public boolean equals(Object other) {
        return other instanceof PointF && equals(((PointF) other).x, ((PointF) other).y);
    }
    @Override public int hashCode() {
        return (x == 0 ? 0 : Float.floatToRawIntBits(x)) * 31 + (y == 0 ? 0 : Float.floatToRawIntBits(y));
    }
    @Override public String toString() { return "PointF(" + x + ", " + y + ")"; }
    public final float length() { return length(x, y); }
    public static float length(float x, float y) { return (float) NativeMath.sqrt((double) x * x + (double) y * y); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) { parcel.writeFloat(x); parcel.writeFloat(y); }
    public void readFromParcel(Parcel parcel) { set(parcel.readFloat(), parcel.readFloat()); }
    public static final Parcelable.Creator<PointF> CREATOR = new Parcelable.Creator<PointF>() {
        public PointF createFromParcel(Parcel parcel) { return new PointF(parcel.readFloat(), parcel.readFloat()); }
        public PointF[] newArray(int size) { return new PointF[size]; }
    };
}
