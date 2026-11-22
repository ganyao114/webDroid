package android.view.accessibility;

import android.view.View;
import java.util.ArrayList;
import java.util.List;

/** Metadata for a node in the embedded view hierarchy. */
public class AccessibilityNodeInfo {
    public static final int ACTION_CLICK = 0x10;
    public static final int ACTION_LONG_CLICK = 0x20;

    public static final class AccessibilityAction {
        public static final AccessibilityAction ACTION_CLICK = new AccessibilityAction(0x10, null);
        public static final AccessibilityAction ACTION_LONG_CLICK = new AccessibilityAction(0x20, null);

        private final int id;
        private final CharSequence label;

        public AccessibilityAction(int actionId, CharSequence label) {
            id = actionId;
            this.label = label;
        }

        public int getId() { return id; }
        public CharSequence getLabel() { return label; }

        @Override public boolean equals(Object other) {
            return other instanceof AccessibilityAction && ((AccessibilityAction) other).id == id;
        }
        @Override public int hashCode() { return id; }
    }

    private final ArrayList<AccessibilityAction> actions = new ArrayList<>();
    private View source;
    private CharSequence className, packageName, contentDescription, stateDescription, text;
    private boolean enabled, visible, clickable, longClickable;

    public AccessibilityNodeInfo() {}
    public AccessibilityNodeInfo(View source) { this.source = source; }
    public static AccessibilityNodeInfo obtain() { return new AccessibilityNodeInfo(); }
    public static AccessibilityNodeInfo obtain(View source) { return new AccessibilityNodeInfo(source); }
    public void recycle() {}
    public void setSource(View source) { this.source = source; }
    public void setClassName(CharSequence name) { className = name; }
    public CharSequence getClassName() { return className; }
    public void setPackageName(CharSequence name) { packageName = name; }
    public CharSequence getPackageName() { return packageName; }
    public void setContentDescription(CharSequence description) { contentDescription = description; }
    public CharSequence getContentDescription() { return contentDescription; }
    public void setStateDescription(CharSequence description) { stateDescription = description; }
    public CharSequence getStateDescription() { return stateDescription; }
    public void setText(CharSequence text) { this.text = text; }
    public CharSequence getText() { return text; }
    public void setEnabled(boolean enabled) { this.enabled = enabled; }
    public boolean isEnabled() { return enabled; }
    public void setVisibleToUser(boolean visible) { this.visible = visible; }
    public boolean isVisibleToUser() { return visible; }
    public void setClickable(boolean clickable) { this.clickable = clickable; }
    public boolean isClickable() { return clickable; }
    public void setLongClickable(boolean longClickable) { this.longClickable = longClickable; }
    public boolean isLongClickable() { return longClickable; }
    public boolean performAction(int action) { return source != null && source.performAccessibilityAction(action, null); }
    public void addAction(int action) { addAction(new AccessibilityAction(action, null)); }
    public void addAction(AccessibilityAction action) {
        if (action == null) return;
        actions.remove(action);
        actions.add(action);
    }
    public boolean removeAction(AccessibilityAction action) { return actions.remove(action); }
    public List<AccessibilityAction> getActionList() { return new ArrayList<>(actions); }
}
