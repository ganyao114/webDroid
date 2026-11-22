package android.animation;

import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

public class LayoutTransition {
    public static final int CHANGE_APPEARING = 0, CHANGE_DISAPPEARING = 1, APPEARING = 2,
            DISAPPEARING = 3, CHANGING = 4;
    public interface TransitionListener {
        void startTransition(LayoutTransition transition, ViewGroup container, View view, int type);
        void endTransition(LayoutTransition transition, ViewGroup container, View view, int type);
    }
    private final ArrayList<TransitionListener> listeners = new ArrayList<>();
    private final TimeInterpolator[] interpolators = new TimeInterpolator[5];
    private int enabled = 0x0f;

    public void enableTransitionType(int type) {
        if (type >= CHANGE_APPEARING && type <= CHANGING) enabled |= 1 << type;
    }
    public void disableTransitionType(int type) {
        if (type >= CHANGE_APPEARING && type <= CHANGING) enabled &= ~(1 << type);
    }
    public boolean isTransitionTypeEnabled(int type) {
        return type >= CHANGE_APPEARING && type <= CHANGING && (enabled & (1 << type)) != 0;
    }
    public void setInterpolator(int type, TimeInterpolator interpolator) {
        if (type >= CHANGE_APPEARING && type <= CHANGING) interpolators[type] = interpolator;
    }
    public TimeInterpolator getInterpolator(int type) {
        return type >= CHANGE_APPEARING && type <= CHANGING ? interpolators[type] : null;
    }
    public void addTransitionListener(TransitionListener listener) { listeners.add(listener); }
    public void removeTransitionListener(TransitionListener listener) { listeners.remove(listener); }
    public boolean isRunning() { return false; }
}
