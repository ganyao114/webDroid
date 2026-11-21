package android.animation;

import java.util.ArrayList;

public abstract class Animator {
    public interface AnimatorListener {
        void onAnimationStart(Animator animation);
        void onAnimationEnd(Animator animation);
        void onAnimationCancel(Animator animation);
        void onAnimationRepeat(Animator animation);
        default void onAnimationStart(Animator animation, boolean reverse) { onAnimationStart(animation); }
        default void onAnimationEnd(Animator animation, boolean reverse) { onAnimationEnd(animation); }
    }
    public interface AnimatorPauseListener {
        void onAnimationPause(Animator animation);
        void onAnimationResume(Animator animation);
    }
    private final ArrayList<AnimatorListener> listeners = new ArrayList<>();
    public void addListener(AnimatorListener listener) { if (listener != null) listeners.add(listener); }
    public void removeListener(AnimatorListener listener) { listeners.remove(listener); }
    public void removeAllListeners() { listeners.clear(); }
    public ArrayList<AnimatorListener> getListeners() { return listeners.isEmpty() ? null : listeners; }
    protected AnimatorListener[] listeners() {
        AnimatorListener[] snapshot = new AnimatorListener[listeners.size()];
        for (int i = 0; i < snapshot.length; i++) snapshot[i] = listeners.get(i);
        return snapshot;
    }
    public abstract void start();
    public abstract void cancel();
    public abstract void end();
    public abstract boolean isRunning();
    public boolean isStarted() { return isRunning(); }
    public abstract long getDuration();
    public abstract Animator setDuration(long milliseconds);
    public abstract long getStartDelay();
    public abstract void setStartDelay(long milliseconds);
    public abstract void setInterpolator(TimeInterpolator interpolator);
    public abstract TimeInterpolator getInterpolator();
}
