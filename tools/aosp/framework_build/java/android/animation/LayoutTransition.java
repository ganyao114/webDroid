package android.animation;

import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

public class LayoutTransition {
    public interface TransitionListener {
        void startTransition(LayoutTransition transition, ViewGroup container, View view, int type);
        void endTransition(LayoutTransition transition, ViewGroup container, View view, int type);
    }
    private final ArrayList<TransitionListener> listeners = new ArrayList<>();
    public void addTransitionListener(TransitionListener listener) { listeners.add(listener); }
    public void removeTransitionListener(TransitionListener listener) { listeners.remove(listener); }
    public boolean isRunning() { return false; }
}
