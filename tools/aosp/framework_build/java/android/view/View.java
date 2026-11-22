package android.view;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.util.FloatProperty;
import android.util.Property;
import android.util.SparseArray;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeIdManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.ArrayList;
import java.util.List;

public class View implements Drawable.Callback, KeyEvent.Callback {
    public static final int NO_ID = -1, VISIBLE = 0, INVISIBLE = 4, GONE = 8;
    public static final int LAYOUT_DIRECTION_LTR = 0, LAYOUT_DIRECTION_RTL = 1, LAYOUT_DIRECTION_INHERIT = 2;
    public static final int FOCUS_BACKWARD = 1, FOCUS_FORWARD = 2, FOCUS_LEFT = 17, FOCUS_UP = 33;
    public static final int FOCUS_RIGHT = 66, FOCUS_DOWN = 130;
    @android.annotation.IntDef({VISIBLE, INVISIBLE, GONE})
    public @interface Visibility {}
    public static final int MEASURED_SIZE_MASK = 0x00ffffff, MEASURED_STATE_MASK = 0xff000000;
    public static final int MEASURED_STATE_TOO_SMALL = 0x01000000, MEASURED_HEIGHT_STATE_SHIFT = 16;
    public static final int SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR = 0x10, SYSTEM_UI_FLAG_LAYOUT_STABLE = 0x100,
            SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 0x200, SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 0x400,
            SYSTEM_UI_FLAG_LIGHT_STATUS_BAR = 0x2000;
    public static final int OVER_SCROLL_ALWAYS = 0, OVER_SCROLL_IF_CONTENT_SCROLLS = 1, OVER_SCROLL_NEVER = 2;
    public static final int LAYER_TYPE_NONE = 0, LAYER_TYPE_SOFTWARE = 1, LAYER_TYPE_HARDWARE = 2;
    protected static final int[] EMPTY_STATE_SET = {};
    protected static final int[] ENABLED_STATE_SET = {android.R.attr.state_enabled};
    protected static final int[] PRESSED_STATE_SET = {android.R.attr.state_pressed};
    protected static final int[] FOCUSED_STATE_SET = {android.R.attr.state_focused};
    protected static final int[] SELECTED_STATE_SET = {android.R.attr.state_selected};

    public static final Property<View, Float> TRANSLATION_X = new FloatProperty<View>("translationX") {
        @Override
        public void setValue(View object, float value) {
            object.setTranslationX(value);
        }

        @Override
        public Float get(View object) {
            return object.getTranslationX();
        }
    };

    public static final Property<View, Float> TRANSLATION_Y = new FloatProperty<View>("translationY") {
        @Override
        public void setValue(View object, float value) {
            object.setTranslationY(value);
        }

        @Override
        public Float get(View object) {
            return object.getTranslationY();
        }
    };

    public static final Property<View, Float> TRANSLATION_Z = new FloatProperty<View>("translationZ") {
        @Override
        public void setValue(View object, float value) {
            object.setTranslationZ(value);
        }

        @Override
        public Float get(View object) {
            return object.getTranslationZ();
        }
    };

    public static final Property<View, Float> SCALE_X = new FloatProperty<View>("scaleX") {
        @Override
        public void setValue(View object, float value) {
            object.setScaleX(value);
        }

        @Override
        public Float get(View object) {
            return object.getScaleX();
        }
    };

    public static final Property<View, Float> SCALE_Y = new FloatProperty<View>("scaleY") {
        @Override
        public void setValue(View object, float value) {
            object.setScaleY(value);
        }

        @Override
        public Float get(View object) {
            return object.getScaleY();
        }
    };

    public static final Property<View, Float> ALPHA = new FloatProperty<View>("alpha") {
        @Override
        public void setValue(View object, float value) {
            object.setAlpha(value);
        }

        @Override
        public Float get(View object) {
            return object.getAlpha();
        }
    };

    public static final Property<View, Float> ROTATION = new FloatProperty<View>("rotation") {
        @Override
        public void setValue(View object, float value) {
            object.setRotation(value);
        }

        @Override
        public Float get(View object) {
            return object.getRotation();
        }
    };

    public static final Property<View, Float> X = new FloatProperty<View>("x") {
        @Override
        public void setValue(View object, float value) {
            object.setX(value);
        }

        @Override
        public Float get(View object) {
            return object.getX();
        }
    };

    public static final Property<View, Float> Y = new FloatProperty<View>("y") {
        @Override
        public void setValue(View object, float value) {
            object.setY(value);
        }

        @Override
        public Float get(View object) {
            return object.getY();
        }
    };

    public static final Property<View, Float> Z = new FloatProperty<View>("z") {
        @Override
        public void setValue(View object, float value) {
            object.setZ(value);
        }

        @Override
        public Float get(View object) {
            return object.getZ();
        }
    };

    private static final int[] ATTRIBUTES = {
        android.R.attr.id, android.R.attr.background, android.R.attr.padding, android.R.attr.paddingLeft,
        android.R.attr.paddingTop, android.R.attr.paddingRight, android.R.attr.paddingBottom,
        android.R.attr.paddingStart, android.R.attr.paddingEnd, android.R.attr.visibility,
        android.R.attr.minWidth, android.R.attr.minHeight, android.R.attr.clickable, android.R.attr.enabled,
        android.R.attr.focusable, android.R.attr.contentDescription, android.R.attr.alpha,
        android.R.attr.paddingHorizontal, android.R.attr.paddingVertical,
    };
    private static int nextViewId = 1;

    public interface OnClickListener { void onClick(View view); }
    public interface OnLongClickListener { boolean onLongClick(View view); }
    public interface OnTouchListener { boolean onTouch(View view, MotionEvent event); }
    public interface OnKeyListener { boolean onKey(View view, int keyCode, KeyEvent event); }
    public interface OnFocusChangeListener { void onFocusChange(View view, boolean focused); }
    public interface OnLayoutChangeListener {
        void onLayoutChange(View v, int l, int t, int r, int b, int oldL, int oldT, int oldR, int oldB);
    }
    public interface OnAttachStateChangeListener {
        void onViewAttachedToWindow(View view);
        void onViewDetachedFromWindow(View view);
    }

    private final Context context;
    public static final int IMPORTANT_FOR_ACCESSIBILITY_AUTO = 0, IMPORTANT_FOR_ACCESSIBILITY_YES = 1,
            IMPORTANT_FOR_ACCESSIBILITY_NO = 2, IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS = 4;
    private int accessibilityImportance;
    private float elevation;
    private float pivotX = Float.NaN, pivotY = Float.NaN;
    private ViewOutlineProvider outlineProvider;
    private Rect clipBounds;
    private int layerType;
    private int transientStateCount;
    private Paint layerPaint;
    private boolean clipToOutline;
    private Boolean forcedOverlappingRendering;
    private int[] drawableState;
    private boolean hapticFeedbackEnabled = true, defaultFocusHighlightEnabled = true;
    private static int nextAccessibilityViewId = 1;
    private int accessibilityViewId = NO_ID;
    private int id = NO_ID, visibility = VISIBLE, layoutDirection = LAYOUT_DIRECTION_INHERIT;
    private int left, top, right, bottom, measuredWidth, measuredHeight;
    private int paddingLeft, paddingTop, paddingRight, paddingBottom, paddingStart, paddingEnd;
    private int minimumWidth, minimumHeight, scrollX, scrollY;
    private boolean layoutRequested = true, enabled = true, clickable, longClickable, focused, focusable;
    private boolean pressed, selected, activated, relativePadding, paddingExplicit, willNotDraw;
    private boolean hasLaidOut, longClickPerformed;
    private boolean focusedByDefault;
    private float translationX, translationY, translationZ, scaleX = 1f, scaleY = 1f, rotation, alpha = 1f;
    private ViewParent parent;
    private ViewRootImpl root;
    private ViewTreeObserver floatingObserver;
    private Handler detachedHandler;
    private ViewGroup.LayoutParams layoutParams;
    private Drawable background, foreground;
    private int systemUiVisibility;
    private List<Rect> gestureExclusion = new ArrayList<>();
    private Object tag;
    private SparseArray<Object> keyedTags;
    private int overScrollMode = OVER_SCROLL_IF_CONTENT_SCROLLS, autofillImportance;
    private boolean scrollContainer;
    private CharSequence contentDescription;
    private OnClickListener clickListener;
    private OnLongClickListener longClickListener;
    private OnTouchListener touchListener;
    private OnKeyListener keyListener;
    private OnFocusChangeListener focusListener;
    private ArrayList<OnLayoutChangeListener> layoutListeners;
    private ArrayList<OnAttachStateChangeListener> attachListeners;
    private final Runnable longPress = () -> {
        if (pressed && enabled && longClickable) longClickPerformed = performLongClick();
    };

    public View(Context context) { this(context, null); }
    public View(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public View(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public View(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        if (context == null) throw new NullPointerException("context");
        this.context = context;
        if (attrs == null && defStyleAttr == 0 && defStyleRes == 0) return;
        try (TypedArray a = context.obtainStyledAttributes(attrs, ATTRIBUTES, defStyleAttr, defStyleRes)) {
            id = a.getResourceId(0, NO_ID);
            if (a.hasValue(1)) setBackground(a.getDrawable(1));
            int horizontal = a.getDimensionPixelSize(17, -1);
            int vertical = a.getDimensionPixelSize(18, -1);
            int all = a.getDimensionPixelSize(2, -1);
            int l = a.getDimensionPixelSize(3, horizontal >= 0 ? horizontal : all >= 0 ? all : paddingLeft);
            int t = a.getDimensionPixelSize(4, vertical >= 0 ? vertical : all >= 0 ? all : paddingTop);
            int r = a.getDimensionPixelSize(5, horizontal >= 0 ? horizontal : all >= 0 ? all : paddingRight);
            int b = a.getDimensionPixelSize(6, vertical >= 0 ? vertical : all >= 0 ? all : paddingBottom);
            if (a.hasValue(7) || a.hasValue(8)) {
                setPaddingRelative(a.getDimensionPixelSize(7, l), t, a.getDimensionPixelSize(8, r), b);
            } else if (all >= 0 || horizontal >= 0 || vertical >= 0 || a.hasValue(3) || a.hasValue(4)
                    || a.hasValue(5) || a.hasValue(6)) setPadding(l, t, r, b);
            visibility = a.getInt(9, 0) * 4;
            minimumWidth = a.getDimensionPixelSize(10, 0);
            minimumHeight = a.getDimensionPixelSize(11, 0);
            clickable = a.getBoolean(12, false);
            enabled = a.getBoolean(13, true);
            drawableState = null;
            focusable = a.getBoolean(14, false);
            contentDescription = a.getText(15);
            alpha = a.getFloat(16, 1f);
            if (background != null && background.isStateful()) background.setState(getDrawableState());
        }
    }

    public final Context getContext() { return context; }
    public static View inflate(Context context, int resource, ViewGroup root) {
        return LayoutInflater.from(context).inflate(resource, root);
    }
    public Resources getResources() { return context.getResources(); }
    public int getId() { return id; }
    public void setId(int id) { this.id = id; }
    public static int generateViewId() {
        int result = nextViewId++;
        if (nextViewId > 0x00ffffff) nextViewId = 1;
        return result;
    }
    @SuppressWarnings("unchecked") public final <T extends View> T findViewById(int id) {
        return id == NO_ID ? null : (T) findViewTraversal(id);
    }
    protected View findViewTraversal(int id) { return this.id == id ? this : null; }
    public void setTag(Object tag) { this.tag = tag; }
    public Object getTag() { return tag; }
    public Object getTag(int key) { return keyedTags == null ? null : keyedTags.get(key); }
    public void setTag(int key, Object value) {
        if ((key >>> 24) < 2) throw new IllegalArgumentException("The key must be an application resource id");
        if (keyedTags == null) keyedTags = new SparseArray<>();
        if (value == null) keyedTags.remove(key);
        else keyedTags.put(key, value);
    }
    public void setScrollContainer(boolean value) { scrollContainer = value; }
    public boolean isScrollContainer() { return scrollContainer; }
    public void setOverScrollMode(int mode) {
        if (mode < OVER_SCROLL_ALWAYS || mode > OVER_SCROLL_NEVER) throw new IllegalArgumentException("Overscroll mode");
        overScrollMode = mode;
    }
    public int getOverScrollMode() { return overScrollMode; }
    public void setImportantForAutofill(int mode) { autofillImportance = mode; }
    public int getImportantForAutofill() { return autofillImportance; }
    public void saveAttributeDataForStyleable(Context context, int[] styleable, AttributeSet attrs,
            TypedArray values, int defStyleAttr, int defStyleRes) {
        // Attribute history is only recorded when an Android layout inspector is attached.
    }
    public <T extends View> T requireViewById(int id) {
        T result = findViewById(id);
        if (result == null) throw new IllegalArgumentException("No view with ID " + id);
        return result;
    }
    public CharSequence getContentDescription() { return contentDescription; }
    public void setContentDescription(CharSequence text) { contentDescription = text; }
    private CharSequence stateDescription;
    public CharSequence getStateDescription() { return stateDescription; }
    public void setStateDescription(CharSequence description) { stateDescription = description; }

    public final ViewParent getParent() { return parent; }
    void assignParent(ViewParent parent) { this.parent = parent; }
    void assignLayoutParams(ViewGroup.LayoutParams params) { layoutParams = params; }
    public View getRootView() {
        View root = this;
        while (root.parent instanceof View) root = (View) root.parent;
        return root;
    }
    public boolean isAttachedToWindow() { return root != null; }
    public boolean hasWindowFocus() { return root != null && root.hasWindowFocus(); }
    public Display getDisplay() { return root == null ? null : context.getDisplay(); }
    public boolean isInEditMode() { return false; }
    public int getWindowVisibility() { return root == null ? GONE : root.getView().getVisibility(); }
    public ViewTreeObserver getViewTreeObserver() {
        if (root != null) return root.getTreeObserver();
        if (floatingObserver == null) floatingObserver = new ViewTreeObserver(context);
        return floatingObserver;
    }
    public android.os.IBinder getWindowToken() { return root == null ? null : root.getWindowToken(); }
    public WindowId getWindowId() { return root == null ? null : root.getWindowId(); }
    public WindowInsets getRootWindowInsets() { return root == null ? null : root.getRootWindowInsets(); }
    public WindowInsetsController getWindowInsetsController() {
        return root == null ? null : root.getWindowInsetsController();
    }
    public void setSystemUiVisibility(int visibility) {
        if (systemUiVisibility == visibility) return;
        systemUiVisibility = visibility;
        if (root != null) root.onSystemUiVisibilityChanged(this);
    }
    public int getSystemUiVisibility() { return systemUiVisibility; }
    public void setSystemGestureExclusionRects(List<Rect> rects) {
        gestureExclusion = copyRects(rects);
    }
    public List<Rect> getSystemGestureExclusionRects() { return copyRects(gestureExclusion); }
    private static List<Rect> copyRects(List<Rect> source) {
        ArrayList<Rect> copy = new ArrayList<>(source.size());
        for (Rect rect : source) copy.add(new Rect(rect));
        return copy;
    }
    void attach(ViewRootImpl root) {
        if (this.root == root) return;
        this.root = root;
        if (accessibilityViewId != NO_ID) {
            AccessibilityNodeIdManager.getInstance().registerViewWithId(this, accessibilityViewId);
        }
        if (floatingObserver != null) {
            root.getTreeObserver().merge(floatingObserver);
            floatingObserver = null;
        }
        onAttachedToWindow();
        if (attachListeners != null) {
            for (OnAttachStateChangeListener listener : new ArrayList<>(attachListeners)) {
                listener.onViewAttachedToWindow(this);
            }
        }
    }
    void detach() {
        if (root == null) return;
        if (pressed) cancelPress();
        if (focused) clearFocus();
        onDetachedFromWindow();
        if (attachListeners != null) {
            for (OnAttachStateChangeListener listener : new ArrayList<>(attachListeners)) {
                listener.onViewDetachedFromWindow(this);
            }
        }
        if (accessibilityViewId != NO_ID) {
            AccessibilityNodeIdManager.getInstance().unregisterViewWithId(accessibilityViewId);
        }
        root = null;
    }
    public void addOnAttachStateChangeListener(OnAttachStateChangeListener listener) {
        if (attachListeners == null) attachListeners = new ArrayList<>();
        attachListeners.add(listener);
    }
    public void removeOnAttachStateChangeListener(OnAttachStateChangeListener listener) {
        if (attachListeners != null) attachListeners.remove(listener);
    }
    protected void onAttachedToWindow() {}
    protected void onDetachedFromWindow() {}
    protected void onFinishInflate() {}
    public void requestApplyInsets() { if (root != null) root.requestApplyInsets(); }
    public WindowInsets dispatchApplyWindowInsets(WindowInsets insets) { return onApplyWindowInsets(insets); }
    public WindowInsets onApplyWindowInsets(WindowInsets insets) { return insets; }
    private WindowInsetsAnimation.Callback insetsAnimationCallback;
    public void setWindowInsetsAnimationCallback(WindowInsetsAnimation.Callback callback) {
        insetsAnimationCallback = callback;
    }
    public void setImportantForAccessibility(int mode) { accessibilityImportance = mode; }
    public int getImportantForAccessibility() { return accessibilityImportance; }
    public boolean isImportantForAccessibility() {
        if (accessibilityImportance == IMPORTANT_FOR_ACCESSIBILITY_NO
                || accessibilityImportance == IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS) return false;
        for (ViewParent owner = parent; owner instanceof View view; owner = view.parent) {
            if (view.accessibilityImportance == IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS) return false;
        }
        return true;
    }
    public boolean includeForAccessibility() { return isImportantForAccessibility(); }
    public boolean isAccessibilityDataSensitive() { return false; }
    public int getAccessibilityViewId() {
        if (accessibilityViewId == NO_ID) {
            synchronized (View.class) {
                accessibilityViewId = nextAccessibilityViewId++;
            }
            AccessibilityNodeIdManager.getInstance().registerViewWithId(this, accessibilityViewId);
        }
        return accessibilityViewId;
    }
    public int getAccessibilityWindowId() { return root == null ? -1 : root.getView().getAccessibilityViewId(); }
    public void setElevation(float value) { if (elevation != value) { elevation = value; invalidate(); } }
    public float getElevation() { return elevation; }
    public void setOutlineProvider(ViewOutlineProvider provider) { outlineProvider = provider; invalidateOutline(); }
    public ViewOutlineProvider getOutlineProvider() { return outlineProvider; }
    public void setClipToOutline(boolean clip) {
        if (clipToOutline != clip) {
            clipToOutline = clip;
            invalidate();
        }
    }
    public boolean getClipToOutline() { return clipToOutline; }
    public void setClipBounds(Rect bounds) {
        if (java.util.Objects.equals(clipBounds, bounds)) return;
        clipBounds = bounds == null ? null : new Rect(bounds);
        invalidate();
    }
    public Rect getClipBounds() { return clipBounds == null ? null : new Rect(clipBounds); }
    public boolean getClipBounds(Rect out) {
        if (clipBounds == null) return false;
        out.set(clipBounds);
        return true;
    }
    public void invalidateOutline() { invalidate(); }
    public void setHapticFeedbackEnabled(boolean enabled) { hapticFeedbackEnabled = enabled; }
    public boolean isHapticFeedbackEnabled() { return hapticFeedbackEnabled; }
    public boolean performHapticFeedback(int effect) { return performHapticFeedback(effect, 0); }
    public boolean performHapticFeedback(int effect, int flags) {
        // Haptic effects require a host vibrator.
        return false;
    }
    public void setDefaultFocusHighlightEnabled(boolean enabled) { defaultFocusHighlightEnabled = enabled; }
    public boolean getDefaultFocusHighlightEnabled() { return defaultFocusHighlightEnabled; }
    public void onVisibilityAggregated(boolean visible) {}
    private AccessibilityDelegate accessibilityDelegate;

    public static class AccessibilityDelegate {
        public void onInitializeAccessibilityEvent(View host, AccessibilityEvent event) {
            host.onInitializeAccessibilityEventInternal(event);
        }
        public boolean dispatchPopulateAccessibilityEvent(View host, AccessibilityEvent event) {
            return host.dispatchPopulateAccessibilityEventInternal(event);
        }
        public void onPopulateAccessibilityEvent(View host, AccessibilityEvent event) {
            host.onPopulateAccessibilityEventInternal(event);
        }
        public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
            host.onInitializeAccessibilityNodeInfoInternal(info);
        }
        public AccessibilityNodeProvider getAccessibilityNodeProvider(View host) { return null; }
        public AccessibilityNodeInfo createAccessibilityNodeInfo(View host) {
            return host.createAccessibilityNodeInfoInternal();
        }
        public boolean performAccessibilityAction(View host, int action, Bundle arguments) {
            return host.performAccessibilityActionInternal(action, arguments);
        }
    }

    public void setAccessibilityDelegate(AccessibilityDelegate delegate) { accessibilityDelegate = delegate; }
    public AccessibilityDelegate getAccessibilityDelegate() { return accessibilityDelegate; }
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return accessibilityDelegate == null ? null : accessibilityDelegate.getAccessibilityNodeProvider(this);
    }
    public AccessibilityNodeInfo createAccessibilityNodeInfo() {
        return accessibilityDelegate == null ? createAccessibilityNodeInfoInternal()
                : accessibilityDelegate.createAccessibilityNodeInfo(this);
    }
    public AccessibilityNodeInfo createAccessibilityNodeInfoInternal() {
        AccessibilityNodeProvider provider = getAccessibilityNodeProvider();
        if (provider != null) return provider.createAccessibilityNodeInfo(AccessibilityNodeProvider.HOST_VIEW_ID);
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(this);
        onInitializeAccessibilityNodeInfo(info);
        return info;
    }
    public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        if (accessibilityDelegate != null) accessibilityDelegate.onInitializeAccessibilityNodeInfo(this, info);
        else onInitializeAccessibilityNodeInfoInternal(info);
    }
    public void onInitializeAccessibilityNodeInfoInternal(AccessibilityNodeInfo info) {
        info.setClassName(getAccessibilityClassName());
        info.setPackageName(context.getPackageName());
        info.setContentDescription(getContentDescription());
        info.setStateDescription(getStateDescription());
        info.setEnabled(isEnabled());
        info.setVisibleToUser(isShown());
        info.setClickable(isClickable());
        info.setLongClickable(isLongClickable());
        if (isClickable()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
        if (isLongClickable()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_LONG_CLICK);
    }
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        return accessibilityDelegate == null ? performAccessibilityActionInternal(action, arguments)
                : accessibilityDelegate.performAccessibilityAction(this, action, arguments);
    }
    public boolean performAccessibilityActionInternal(int action, Bundle arguments) {
        if (!isEnabled()) return false;
        if (action == AccessibilityNodeInfo.ACTION_CLICK && isClickable()) return performClick();
        return action == AccessibilityNodeInfo.ACTION_LONG_CLICK && isLongClickable() && performLongClick();
    }
    public void onInitializeAccessibilityEvent(AccessibilityEvent event) {
        if (accessibilityDelegate != null) accessibilityDelegate.onInitializeAccessibilityEvent(this, event);
        else onInitializeAccessibilityEventInternal(event);
    }
    public void onInitializeAccessibilityEventInternal(AccessibilityEvent event) {
        event.setClassName(getAccessibilityClassName());
        event.setPackageName(context.getPackageName());
        event.setSource(this);
    }
    public CharSequence getAccessibilityClassName() { return getClass().getName(); }
    public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) {
        return accessibilityDelegate == null ? dispatchPopulateAccessibilityEventInternal(event)
                : accessibilityDelegate.dispatchPopulateAccessibilityEvent(this, event);
    }
    public boolean dispatchPopulateAccessibilityEventInternal(android.view.accessibility.AccessibilityEvent event) {
        onPopulateAccessibilityEvent(event);
        return false;
    }
    public void onPopulateAccessibilityEvent(AccessibilityEvent event) {
        if (accessibilityDelegate != null) accessibilityDelegate.onPopulateAccessibilityEvent(this, event);
        else onPopulateAccessibilityEventInternal(event);
    }
    public void onPopulateAccessibilityEventInternal(AccessibilityEvent event) {}
    public void sendAccessibilityEvent(int eventType) {
        var manager = context.getSystemService(android.view.accessibility.AccessibilityManager.class);
        if (manager == null || !manager.isEnabled() || !isShown()) return;
        AccessibilityEvent event = AccessibilityEvent.obtain(eventType);
        onInitializeAccessibilityEvent(event);
        dispatchPopulateAccessibilityEvent(event);
        manager.sendAccessibilityEvent(event);
    }
    public void announceForAccessibility(CharSequence message) {
        var manager = context.getSystemService(android.view.accessibility.AccessibilityManager.class);
        if (manager == null || !manager.isEnabled()) return;
        var event = android.view.accessibility.AccessibilityEvent.obtain(16384);
        event.getText().add(message);
        onInitializeAccessibilityEvent(event);
        manager.sendAccessibilityEvent(event);
    }
    public void sendAccessibilityEventUnchecked(android.view.accessibility.AccessibilityEvent event) {
        var manager = context.getSystemService(android.view.accessibility.AccessibilityManager.class);
        if (manager != null) manager.sendAccessibilityEvent(event);
    }
    public void getLocationInWindow(int[] location) {
        if (location == null || location.length < 2) throw new IllegalArgumentException("location");
        float x = 0, y = 0;
        for (View view = this; view != null;) {
            float px = view.getPivotX(), py = view.getPivotY();
            float sx = (x - px) * view.getScaleX(), sy = (y - py) * view.getScaleY();
            double radians = Math.toRadians(view.getRotation());
            float cos = (float) Math.cos(radians), sin = (float) Math.sin(radians);
            x = view.getX() + px + sx * cos - sy * sin;
            y = view.getY() + py + sx * sin + sy * cos;
            ViewParent owner = view.getParent();
            view = owner instanceof View ? (View) owner : null;
            if (view != null) { x -= view.getScrollX(); y -= view.getScrollY(); }
        }
        location[0] = org.wasmandroid.ui.NativeMath.round(x);
        location[1] = org.wasmandroid.ui.NativeMath.round(y);
    }
    public void getLocationOnScreen(int[] location) {
        // The embedded window and the SurfaceFlinger viewport share an origin.
        getLocationInWindow(location);
    }

    public ViewGroup.LayoutParams getLayoutParams() { return layoutParams; }
    public void setLayoutParams(ViewGroup.LayoutParams params) {
        if (params == null) throw new NullPointerException("layoutParams");
        layoutParams = params;
        requestLayout();
    }
    public void requestLayout() {
        layoutRequested = true;
        if (parent != null && !parent.isLayoutRequested()) parent.requestLayout();
        if (root != null) root.requestLayout();
    }
    public void forceLayout() { layoutRequested = true; }
    public boolean isLayoutRequested() { return layoutRequested; }
    public void invalidate() {
        if (parent != null) parent.onDescendantInvalidated(this, this);
    }
    public void invalidate(int left, int top, int right, int bottom) { invalidate(); }
    public void invalidate(Rect dirty) { invalidate(); }
    public void postInvalidate() { invalidate(); }
    public void postInvalidateOnAnimation() { invalidate(); }
    public void postInvalidateDelayed(long delay) { postDelayed(this::invalidate, delay); }

    public static final class MeasureSpec {
        private static final int MODE_MASK = 0xc0000000;
        public static final int UNSPECIFIED = 0, EXACTLY = 0x40000000, AT_MOST = 0x80000000;
        private MeasureSpec() {}
        public static int makeMeasureSpec(int size, int mode) { return (size & ~MODE_MASK) | (mode & MODE_MASK); }
        public static int getMode(int spec) { return spec & MODE_MASK; }
        public static int getSize(int spec) { return spec & ~MODE_MASK; }
        public static String toString(int spec) {
            return "MeasureSpec " + (getMode(spec) == EXACTLY ? "EXACTLY " :
                    getMode(spec) == AT_MOST ? "AT_MOST " : "UNSPECIFIED ") + getSize(spec);
        }
    }
    public final void measure(int widthSpec, int heightSpec) { onMeasure(widthSpec, heightSpec); }
    protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(getDefaultSize(getSuggestedMinimumWidth(), widthSpec),
                getDefaultSize(getSuggestedMinimumHeight(), heightSpec));
    }
    protected final void setMeasuredDimension(int width, int height) { measuredWidth = width; measuredHeight = height; }
    public final int getMeasuredWidth() { return measuredWidth & MEASURED_SIZE_MASK; }
    public final int getMeasuredHeight() { return measuredHeight & MEASURED_SIZE_MASK; }
    public final int getMeasuredWidthAndState() { return measuredWidth; }
    public final int getMeasuredHeightAndState() { return measuredHeight; }
    public final int getMeasuredState() {
        return (measuredWidth & MEASURED_STATE_MASK)
                | ((measuredHeight & MEASURED_STATE_MASK) >>> MEASURED_HEIGHT_STATE_SHIFT);
    }
    public static int combineMeasuredStates(int current, int next) { return current | next; }
    public static int getDefaultSize(int desired, int spec) {
        return MeasureSpec.getMode(spec) == MeasureSpec.UNSPECIFIED ? desired : MeasureSpec.getSize(spec);
    }
    public static int resolveSize(int size, int spec) { return resolveSizeAndState(size, spec, 0) & MEASURED_SIZE_MASK; }
    public static int resolveSizeAndState(int size, int spec, int childState) {
        int limit = MeasureSpec.getSize(spec);
        int result = size;
        if (MeasureSpec.getMode(spec) == MeasureSpec.EXACTLY) result = limit;
        else if (MeasureSpec.getMode(spec) == MeasureSpec.AT_MOST && size > limit) result = limit | MEASURED_STATE_TOO_SMALL;
        return result | (childState & MEASURED_STATE_MASK);
    }

    public void layout(int left, int top, int right, int bottom) {
        int l = this.left, t = this.top, r = this.right, b = this.bottom;
        boolean changed = l != left || t != top || r != right || b != bottom;
        boolean needsLayout = changed || layoutRequested || !hasLaidOut;
        int oldWidth = getWidth(), oldHeight = getHeight();
        this.left = left; this.top = top; this.right = right; this.bottom = bottom;
        layoutRequested = false;
        if (oldWidth != getWidth() || oldHeight != getHeight()) onSizeChanged(getWidth(), getHeight(), oldWidth, oldHeight);
        if (needsLayout) {
            onLayout(changed, left, top, right, bottom);
            if (layoutListeners != null) {
                OnLayoutChangeListener[] snapshot = new OnLayoutChangeListener[layoutListeners.size()];
                for (int i = 0; i < snapshot.length; i++) snapshot[i] = layoutListeners.get(i);
                for (OnLayoutChangeListener listener : snapshot) listener.onLayoutChange(this, left, top, right, bottom, l, t, r, b);
            }
        }
        hasLaidOut = true;
    }
    protected void onLayout(boolean changed, int l, int t, int r, int b) {}
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {}
    public void addOnLayoutChangeListener(OnLayoutChangeListener listener) {
        if (layoutListeners == null) layoutListeners = new ArrayList<>();
        if (!layoutListeners.contains(listener)) layoutListeners.add(listener);
    }
    public void removeOnLayoutChangeListener(OnLayoutChangeListener listener) {
        if (layoutListeners != null) layoutListeners.remove(listener);
    }
    public boolean isLaidOut() { return hasLaidOut; }
    public final int getLeft() { return left; }
    public final int getTop() { return top; }
    public final int getRight() { return right; }
    public final int getBottom() { return bottom; }
    public void offsetTopAndBottom(int offset) {
        if (offset == 0) return;
        top += offset;
        bottom += offset;
        invalidate();
    }
    public void offsetLeftAndRight(int offset) {
        if (offset == 0) return;
        left += offset;
        right += offset;
        invalidate();
    }
    public final int getWidth() { return right - left; }
    public final int getHeight() { return bottom - top; }
    public float getX() { return left + translationX; }
    public float getY() { return top + translationY; }
    private Matrix transformMatrix;
    public Matrix getMatrix() {
        if (transformMatrix == null) transformMatrix = new Matrix();
        double angle = Math.toRadians(getRotation());
        float cosine = (float) Math.cos(angle), sine = (float) Math.sin(angle);
        float a = cosine * scaleX, b = -sine * scaleY, c = sine * scaleX, d = cosine * scaleY;
        float px = getPivotX(), py = getPivotY();
        transformMatrix.setValues(new float[] {a, b, translationX + px - a * px - b * py,
                c, d, translationY + py - c * px - d * py, 0, 0, 1});
        return transformMatrix;
    }
    public float getZ() { return getElevation() + getTranslationZ(); }
    public void setZ(float z) { setTranslationZ(z - getElevation()); }
    public void setX(float x) { setTranslationX(x - left); }
    public void setY(float y) { setTranslationY(y - top); }
    public float getTranslationX() { return translationX; }
    public float getTranslationY() { return translationY; }
    public float getTranslationZ() { return translationZ; }
    public void setTranslationZ(float z) { if (translationZ != z) { translationZ = z; invalidate(); } }
    public void setTranslationX(float x) { if (translationX != x) { translationX = x; invalidate(); } }
    public void setTranslationY(float y) { if (translationY != y) { translationY = y; invalidate(); } }
    public float getScaleX() { return scaleX; }
    public float getScaleY() { return scaleY; }
    public void setScaleX(float scale) { if (scaleX != scale) { scaleX = scale; invalidate(); } }
    public void setScaleY(float scale) { if (scaleY != scale) { scaleY = scale; invalidate(); } }
    public float getPivotX() { return Float.isNaN(pivotX) ? getWidth() * 0.5f : pivotX; }
    public float getPivotY() { return Float.isNaN(pivotY) ? getHeight() * 0.5f : pivotY; }
    public void setPivotX(float x) { pivotX = x; invalidate(); }
    public void setPivotY(float y) { pivotY = y; invalidate(); }
    public float getRotation() { return rotation; }
    public void setRotation(float rotation) { this.rotation = rotation; invalidate(); }
    public float getAlpha() { return alpha; }
    public boolean hasTransientState() { return transientStateCount > 0; }
    public void setHasTransientState(boolean transientState) {
        transientStateCount = Math.max(0, transientStateCount + (transientState ? 1 : -1));
    }
    public void clearAnimation() {
        // Property animators track their own lifecycle.
    }
    public void forceHasOverlappingRendering(boolean overlaps) { forcedOverlappingRendering = overlaps; }
    public boolean hasOverlappingRendering() { return true; }
    public final boolean getHasOverlappingRendering() {
        return forcedOverlappingRendering == null ? hasOverlappingRendering() : forcedOverlappingRendering;
    }
    public void setAlpha(float alpha) {
        float value = Math.max(0, Math.min(1, alpha));
        if (this.alpha != value) { this.alpha = value; invalidate(); }
    }
    public void getHitRect(Rect out) { out.set(left, top, right, bottom); }
    public void getDrawingRect(Rect out) { out.set(scrollX, scrollY, scrollX + getWidth(), scrollY + getHeight()); }
    public void getFocusedRect(Rect out) { getDrawingRect(out); }
    public boolean requestRectangleOnScreen(Rect rectangle) { return requestRectangleOnScreen(rectangle, false); }
    public boolean requestRectangleOnScreen(Rect rectangle, boolean immediate) {
        if (parent == null) return false;
        Rect position = new Rect(rectangle);
        Rect requested = new Rect();
        boolean scrolled = false;
        View child = this;
        while (child.parent != null) {
            ViewParent owner = child.parent;
            requested.set(position);
            scrolled |= owner.requestChildRectangleOnScreen(child, requested, immediate);
            if (!(owner instanceof View view)) break;
            position.offset(child.left - child.scrollX, child.top - child.scrollY);
            child = view;
        }
        return scrolled;
    }
    public final boolean getLocalVisibleRect(Rect out) {
        Point origin = new Point();
        if (!getGlobalVisibleRect(out, origin)) return false;
        out.offset(-origin.x, -origin.y);
        return true;
    }
    public final boolean getGlobalVisibleRect(Rect out) { return getGlobalVisibleRect(out, null); }
    public boolean getGlobalVisibleRect(Rect out, Point origin) {
        int width = getWidth(), height = getHeight();
        if (width <= 0 || height <= 0) return false;
        out.set(0, 0, width, height);
        if (origin != null) origin.set(-scrollX, -scrollY);
        return mapVisibleRectToWindow(out, origin);
    }
    boolean mapVisibleRectToWindow(Rect out, Point origin) {
        RectF bounds = new RectF(out);
        for (View child = this; child.parent instanceof ViewGroup group; child = group) {
            float dx = child.getX() - group.getScrollX(), dy = child.getY() - group.getScrollY();
            double angle = Math.toRadians(child.getRotation());
            float cosine = (float) Math.cos(angle), sine = (float) Math.sin(angle);
            float a = cosine * child.getScaleX(), b = -sine * child.getScaleY();
            float c = sine * child.getScaleX(), d = cosine * child.getScaleY();
            float px = child.getPivotX(), py = child.getPivotY();
            dx += px - a * px - b * py;
            dy += py - c * px - d * py;
            float x0 = a * bounds.left, x1 = a * bounds.right, x2 = b * bounds.top, x3 = b * bounds.bottom;
            float y0 = c * bounds.left, y1 = c * bounds.right, y2 = d * bounds.top, y3 = d * bounds.bottom;
            bounds.set(Math.min(x0, x1) + Math.min(x2, x3) + dx, Math.min(y0, y1) + Math.min(y2, y3) + dy,
                    Math.max(x0, x1) + Math.max(x2, x3) + dx, Math.max(y0, y1) + Math.max(y2, y3) + dy);
            if (origin != null) {
                origin.set(org.wasmandroid.ui.NativeMath.round(a * origin.x + b * origin.y + dx),
                        org.wasmandroid.ui.NativeMath.round(c * origin.x + d * origin.y + dy));
            }
            int parentWidth = group.getWidth(), parentHeight = group.getHeight();
            if ((!(group.getParent() instanceof ViewGroup grandparent) || grandparent.getClipChildren())
                    && !bounds.intersect(0, 0, parentWidth, parentHeight)) return false;
            if (group.getClipToPadding() && !bounds.intersect(group.getPaddingLeft(), group.getPaddingTop(),
                    parentWidth - group.getPaddingRight(), parentHeight - group.getPaddingBottom())) return false;
        }
        bounds.roundOut(out);
        return !out.isEmpty();
    }
    public int getBaseline() { return -1; }
    public int getVisibility() { return visibility; }
    public void setVisibility(int visibility) {
        if (this.visibility == visibility) return;
        this.visibility = visibility;
        if (visibility != VISIBLE) { setPressed(false); clearFocus(); }
        requestLayout();
        invalidate();
    }
    public boolean isShown() {
        if (visibility != VISIBLE || root == null) return false;
        return !(parent instanceof View) || ((View) parent).isShown();
    }
    public void setMinimumWidth(int width) { minimumWidth = width; requestLayout(); }
    public void setMinimumHeight(int height) { minimumHeight = height; requestLayout(); }
    public int getMinimumWidth() { return minimumWidth; }
    public int getMinimumHeight() { return minimumHeight; }
    protected int getSuggestedMinimumWidth() { return Math.max(minimumWidth, background == null ? 0 : background.getMinimumWidth()); }
    protected int getSuggestedMinimumHeight() { return Math.max(minimumHeight, background == null ? 0 : background.getMinimumHeight()); }
    public void setBackground(Drawable drawable) {
        if (background == drawable) return;
        if (background != null) background.setCallback(null);
        background = drawable;
        if (drawable != null) {
            drawable.setCallback(this);
            drawable.setState(getDrawableState());
            Rect padding = new Rect();
            if (!paddingExplicit && drawable.getPadding(padding)) {
                applyPadding(padding.left, padding.top, padding.right, padding.bottom);
            }
        }
        requestLayout();
        invalidate();
    }
    @Deprecated public void setBackgroundDrawable(Drawable drawable) { setBackground(drawable); }
    public void setBackgroundColor(int color) { setBackground(new ColorDrawable(color)); }
    public void setBackgroundResource(int id) { setBackground(id == 0 ? null : context.getDrawable(id)); }
    public Drawable getBackground() { return background; }
    public void setForeground(Drawable drawable) {
        if (foreground == drawable) return;
        if (foreground != null) foreground.setCallback(null);
        foreground = drawable;
        if (drawable != null) {
            drawable.setCallback(this);
            drawable.setState(getDrawableState());
        }
        invalidate();
    }
    public Drawable getForeground() { return foreground; }
    public void setPadding(int l, int t, int r, int b) {
        relativePadding = false; paddingExplicit = true;
        applyPadding(l, t, r, b);
    }
    public void setPaddingRelative(int start, int top, int end, int bottom) {
        relativePadding = true; paddingExplicit = true;
        paddingStart = start; paddingEnd = end;
        applyPadding(isLayoutRtl() ? end : start, top, isLayoutRtl() ? start : end, bottom);
    }
    private void applyPadding(int l, int t, int r, int b) {
        if (paddingLeft == l && paddingTop == t && paddingRight == r && paddingBottom == b) return;
        paddingLeft = l; paddingTop = t; paddingRight = r; paddingBottom = b;
        requestLayout();
    }
    public int getPaddingLeft() { return relativePadding ? (isLayoutRtl() ? paddingEnd : paddingStart) : paddingLeft; }
    public int getPaddingRight() { return relativePadding ? (isLayoutRtl() ? paddingStart : paddingEnd) : paddingRight; }
    public int getPaddingTop() { return paddingTop; }
    public int getPaddingBottom() { return paddingBottom; }
    public int getPaddingStart() { return isLayoutRtl() ? getPaddingRight() : getPaddingLeft(); }
    public int getPaddingEnd() { return isLayoutRtl() ? getPaddingLeft() : getPaddingRight(); }
    public boolean isPaddingRelative() { return relativePadding; }
    public void setLayoutDirection(int direction) { layoutDirection = direction; requestLayout(); }
    public int getLayoutDirection() {
        return layoutDirection != LAYOUT_DIRECTION_INHERIT ? layoutDirection : parent instanceof View
                ? ((View) parent).getLayoutDirection() : getResources().getConfiguration().getLayoutDirection();
    }
    public boolean isLayoutRtl() { return getLayoutDirection() == LAYOUT_DIRECTION_RTL; }
    public void setWillNotDraw(boolean value) { willNotDraw = value; invalidate(); }
    public boolean willNotDraw() { return willNotDraw; }
    public boolean isHardwareAccelerated() { return isAttachedToWindow(); }
    public void setLayerType(int type, Paint paint) {
        if (type < LAYER_TYPE_NONE || type > LAYER_TYPE_HARDWARE) throw new IllegalArgumentException("Layer type");
        layerType = type;
        layerPaint = type == LAYER_TYPE_NONE ? null : paint;
        invalidate();
    }
    public int getLayerType() { return layerType; }
    public void buildLayer() { if (layerType != LAYER_TYPE_NONE) invalidate(); }
    public void draw(Canvas canvas) {
        int saved = canvas.getSaveCount();
        try {
            if (layerPaint != null && layerPaint.getAlpha() != 255) {
                canvas.save();
                canvas.multiplyAlpha(layerPaint.getAlpha() / 255f);
            }
            if (clipBounds != null) {
                canvas.save();
                canvas.clipRect(clipBounds);
            }
            if (clipToOutline && outlineProvider != null) {
                Outline outline = new Outline();
                outlineProvider.getOutline(this, outline);
                Rect bounds = new Rect();
                if (outline.getRect(bounds)) {
                    canvas.save();
                    float radius = outline.getRadius();
                    if (radius == 0) canvas.clipRect(bounds);
                    else {
                        Path path = new Path();
                        path.addRoundRect(bounds.left, bounds.top, bounds.right, bounds.bottom,
                                radius, radius, Path.Direction.CW);
                        canvas.clipPath(path);
                    }
                }
            }
            if (background != null) {
                background.setBounds(0, 0, getWidth(), getHeight());
                background.draw(canvas);
            }
            computeScroll();
            int scrolled = canvas.getSaveCount();
            if (scrollX != 0 || scrollY != 0) {
                canvas.save();
                canvas.translate(-scrollX, -scrollY);
            }
            if (!willNotDraw) onDraw(canvas);
            dispatchDraw(canvas);
            canvas.restoreToCount(scrolled);
            if (foreground != null) {
                foreground.setBounds(0, 0, getWidth(), getHeight());
                foreground.draw(canvas);
            }
        } finally {
            canvas.restoreToCount(saved);
        }
    }
    protected void onDraw(Canvas canvas) {}
    protected void dispatchDraw(Canvas canvas) {}
    public void computeScroll() {}
    public final int getScrollX() { return scrollX; }
    public final int getScrollY() { return scrollY; }
    public void scrollTo(int x, int y) {
        if (scrollX == x && scrollY == y) return;
        int oldX = scrollX, oldY = scrollY;
        scrollX = x; scrollY = y;
        onScrollChanged(x, y, oldX, oldY);
        invalidate();
    }
    public void scrollBy(int x, int y) { scrollTo(scrollX + x, scrollY + y); }
    protected void onScrollChanged(int x, int y, int oldX, int oldY) {}

    public Handler getHandler() { return root == null ? null : root.getHandler(); }
    private Handler postingHandler() {
        if (root != null) return root.getHandler();
        if (detachedHandler == null) detachedHandler = new Handler(context.getMainLooper());
        return detachedHandler;
    }
    public boolean post(Runnable runnable) { return postingHandler().post(runnable); }
    public boolean postDelayed(Runnable runnable, long delay) { return postingHandler().postDelayed(runnable, delay); }
    public void postOnAnimation(Runnable runnable) { Choreographer.getInstance().postCallback(Choreographer.CALLBACK_ANIMATION, runnable, this); }
    public void postOnAnimationDelayed(Runnable runnable, long delay) {
        Choreographer.getInstance().postCallbackDelayed(Choreographer.CALLBACK_ANIMATION, runnable, this, delay);
    }
    public boolean removeCallbacks(Runnable runnable) {
        if (runnable != null) {
            if (root != null) root.getHandler().removeCallbacks(runnable);
            if (detachedHandler != null) detachedHandler.removeCallbacks(runnable);
            Choreographer.getInstance().removeCallbacks(Choreographer.CALLBACK_ANIMATION, runnable, this);
        }
        return true;
    }
    @Override public void invalidateDrawable(Drawable drawable) { invalidate(); }
    @Override public void scheduleDrawable(Drawable drawable, Runnable task, long when) {
        postDelayed(task, Math.max(0, when - SystemClock.uptimeMillis()));
    }
    @Override public void unscheduleDrawable(Drawable drawable, Runnable task) { removeCallbacks(task); }

    public void setOnClickListener(OnClickListener listener) { clickListener = listener; if (listener != null) setClickable(true); }
    public void setOnLongClickListener(OnLongClickListener listener) { longClickListener = listener; if (listener != null) setLongClickable(true); }
    public boolean hasOnClickListeners() { return clickListener != null; }
    public void setOnTouchListener(OnTouchListener listener) { touchListener = listener; }
    public void setOnKeyListener(OnKeyListener listener) { keyListener = listener; }
    public void setOnFocusChangeListener(OnFocusChangeListener listener) { focusListener = listener; }
    public boolean isClickable() { return clickable; }
    public void setClickable(boolean value) { clickable = value; }
    public boolean isLongClickable() { return longClickable; }
    public void setLongClickable(boolean value) { longClickable = value; }
    public boolean isEnabled() { return enabled; }
    public void setEnabled(boolean value) {
        if (enabled == value) return;
        enabled = value;
        if (!value) setPressed(false);
        refreshDrawableState();
    }
    public boolean isPressed() { return pressed; }
    public void setPressed(boolean value) { if (pressed != value) { pressed = value; refreshDrawableState(); } }
    public boolean isActivated() { return activated; }
    public void setActivated(boolean value) { if (activated != value) { activated = value; refreshDrawableState(); } }
    public boolean isSelected() { return selected; }
    public void setSelected(boolean value) { if (selected != value) { selected = value; refreshDrawableState(); } }
    public void setFocusable(boolean value) { focusable = value; if (!value) clearFocus(); }
    public boolean isFocusable() { return focusable; }
    public boolean hasFocusable() { return isEnabled() && getVisibility() == VISIBLE && isFocusable(); }
    public void setFocusableInTouchMode(boolean value) { if (value) setFocusable(true); }
    public boolean isFocused() { return focused; }
    public void setFocusedByDefault(boolean value) { focusedByDefault = value; }
    public boolean isFocusedByDefault() { return focusedByDefault; }
    public boolean restoreDefaultFocus() { return requestFocus(FOCUS_FORWARD); }
    public boolean hasFocus() { return focused; }
    public View findFocus() { return focused ? this : null; }
    public final boolean requestFocus() { return requestFocus(FOCUS_DOWN); }
    public final boolean requestFocus(int direction) { return requestFocus(direction, null); }
    public boolean requestFocus(int direction, Rect oldRect) {
        if (!focusable || !enabled || visibility != VISIBLE) return false;
        if (parent != null) parent.requestChildFocus(this, this);
        setFocused(true, direction, oldRect);
        return true;
    }
    void setFocused(boolean focused, int direction, Rect oldRect) {
        if (this.focused == focused) return;
        this.focused = focused;
        refreshDrawableState();
        onFocusChanged(focused, direction, oldRect);
        if (focusListener != null) focusListener.onFocusChange(this, focused);
    }
    protected void onFocusChanged(boolean focused, int direction, Rect oldRect) {}
    public void clearFocus() {
        if (!focused) return;
        setFocused(false, 0, null);
        if (parent != null) parent.clearChildFocus(this);
    }
    public int[] getDrawableState() {
        if (drawableState != null) return drawableState;
        int[] result = new int[(enabled ? 1 : 0) + (pressed ? 1 : 0)
                + (selected ? 1 : 0) + (focused ? 1 : 0) + (activated ? 1 : 0)];
        int i = 0;
        if (enabled) result[i++] = android.R.attr.state_enabled;
        if (pressed) result[i++] = android.R.attr.state_pressed;
        if (selected) result[i++] = android.R.attr.state_selected;
        if (focused) result[i++] = android.R.attr.state_focused;
        if (activated) result[i] = android.R.attr.state_activated;
        return drawableState = result;
    }
    public void refreshDrawableState() { drawableState = null; drawableStateChanged(); invalidate(); }
    protected void drawableStateChanged() {
        if (background != null && background.isStateful()) background.setState(getDrawableState());
        if (foreground != null && foreground.isStateful()) foreground.setState(getDrawableState());
    }

    public boolean performClick() {
        if (clickListener == null) return false;
        clickListener.onClick(this);
        return true;
    }
    public boolean callOnClick() { return performClick(); }
    public boolean performLongClick() { return longClickListener != null && longClickListener.onLongClick(this); }
    public boolean dispatchTouchEvent(MotionEvent event) {
        return (enabled && touchListener != null && touchListener.onTouch(this, event)) || onTouchEvent(event);
    }
    public boolean onTouchEvent(MotionEvent event) {
        if (!clickable && !longClickable) return false;
        if (!enabled) return true;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                longClickPerformed = false;
                setPressed(true);
                if (longClickable) postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                break;
            case MotionEvent.ACTION_MOVE:
                int slop = ViewConfiguration.get(context).getScaledTouchSlop();
                float x = event.getX(), y = event.getY();
                if (x < -slop || y < -slop || x >= getWidth() + slop || y >= getHeight() + slop) cancelPress();
                break;
            case MotionEvent.ACTION_UP:
                boolean click = pressed && !longClickPerformed;
                cancelPress();
                if (click) performClick();
                break;
            case MotionEvent.ACTION_CANCEL: cancelPress(); break;
        }
        return true;
    }
    public void cancelLongPress() { removeCallbacks(longPress); }
    protected boolean awakenScrollBars() { return awakenScrollBars(ViewConfiguration.getScrollDefaultDelay()); }
    protected boolean awakenScrollBars(int delay) { return awakenScrollBars(delay, true); }
    protected boolean awakenScrollBars(int delay, boolean invalidate) { return false; }
    private void cancelPress() { cancelLongPress(); setPressed(false); }
    public boolean dispatchGenericMotionEvent(MotionEvent event) { return onGenericMotionEvent(event); }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
    public boolean dispatchKeyEvent(KeyEvent event) {
        return (enabled && keyListener != null && keyListener.onKey(this, event.getKeyCode(), event)) || event.dispatch(this);
    }
    public boolean onKeyDown(int code, KeyEvent event) {
        if (enabled && clickable && confirmKey(code)) { setPressed(true); return true; }
        return false;
    }
    public boolean onKeyUp(int code, KeyEvent event) {
        if (pressed && confirmKey(code)) {
            setPressed(false);
            return event.isCanceled() || performClick();
        }
        return false;
    }
    private static boolean confirmKey(int code) {
        return code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER || code == KeyEvent.KEYCODE_SPACE;
    }
    public boolean onKeyLongPress(int code, KeyEvent event) { return false; }
    public boolean onKeyMultiple(int code, int count, KeyEvent event) { return false; }
    public boolean onImeText(CharSequence text, boolean composing) { return false; }
}
