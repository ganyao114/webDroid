package android.graphics;

import android.os.Parcel;
import android.os.Parcelable;

public class Point implements Parcelable {
    public int x, y;
    public Point() {}
    public Point(int x, int y) { set(x, y); }
    public Point(Point other) { set(other.x, other.y); }
    public void set(int x, int y) { this.x = x; this.y = y; }
    public final void negate() { set(-x, -y); }
    public final void offset(int x, int y) { set(this.x + x, this.y + y); }
    public final boolean equals(int x, int y) { return this.x == x && this.y == y; }
    @Override public boolean equals(Object other) {
        return other instanceof Point && equals(((Point) other).x, ((Point) other).y);
    }
    @Override public int hashCode() { return x * 31 + y; }
    @Override public String toString() { return "Point(" + x + ", " + y + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel parcel, int flags) { parcel.writeInt(x); parcel.writeInt(y); }
    public void readFromParcel(Parcel parcel) { set(parcel.readInt(), parcel.readInt()); }
    public static final Parcelable.Creator<Point> CREATOR = new Parcelable.Creator<Point>() {
        public Point createFromParcel(Parcel parcel) { return new Point(parcel.readInt(), parcel.readInt()); }
        public Point[] newArray(int size) { return new Point[size]; }
    };
}
