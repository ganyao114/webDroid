package android.view.accessibility;

import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

public class AccessibilityEvent {
    public static final int TYPE_VIEW_FOCUSED = 8, TYPE_VIEW_SCROLLED = 4096, TYPE_ANNOUNCEMENT = 16384;
    private final int type;
    private final List<CharSequence> text = new ArrayList<>();
    private boolean scrollable;
    private int scrollX, scrollY, maxScrollX, maxScrollY, itemCount, fromIndex, toIndex;
    private CharSequence className, packageName;
    private Parcelable data;
    private AccessibilityEvent(int type) { this.type = type; }
    public static AccessibilityEvent obtain(int type) { return new AccessibilityEvent(type); }
    public int getEventType() { return type; }
    public List<CharSequence> getText() { return text; }
    public void setScrollable(boolean value) { scrollable = value; }
    public void setScrollX(int value) { scrollX = value; }
    public void setScrollY(int value) { scrollY = value; }
    public void setMaxScrollX(int value) { maxScrollX = value; }
    public void setMaxScrollY(int value) { maxScrollY = value; }
    public void setItemCount(int value) { itemCount = value; }
    public void setFromIndex(int value) { fromIndex = value; }
    public void setToIndex(int value) { toIndex = value; }
    public void setClassName(CharSequence name) { className = name; }
    public void setPackageName(CharSequence name) { packageName = name; }
    public void setParcelableData(Parcelable value) { data = value; }
}
