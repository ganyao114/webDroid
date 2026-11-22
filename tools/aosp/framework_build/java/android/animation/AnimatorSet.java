package android.animation;

import android.view.Choreographer;
import java.util.ArrayList;
import java.util.Collection;

/** Runs a group of animations on the same cooperative frame clock as ValueAnimator. */
public final class AnimatorSet extends Animator implements Choreographer.FrameCallback {
    private final ArrayList<Animator> children = new ArrayList<>();
    private long duration = -1, delay;
    private TimeInterpolator interpolator;
    private boolean started, running;

    public final class Builder {
        public Builder with(Animator animator) {
            add(animator);
            return this;
        }
    }

    public Builder play(Animator animator) {
        if (animator == null) return null;
        add(animator);
        return new Builder();
    }
    public void playTogether(Animator... animators) {
        for (Animator animator : animators) add(animator);
    }
    public void playTogether(Collection<Animator> animators) {
        for (Animator animator : animators) add(animator);
    }
    private void add(Animator animator) {
        if (animator == this) throw new IllegalArgumentException("Cannot group an animation with itself");
        if (animator != null && !children.contains(animator)) children.add(animator);
    }
    public ArrayList<Animator> getChildAnimations() { return new ArrayList<>(children); }
    @Override public AnimatorSet setDuration(long milliseconds) {
        if (milliseconds < 0) throw new IllegalArgumentException("Negative animation duration");
        duration = milliseconds;
        return this;
    }
    @Override public long getDuration() { return duration; }
    @Override public void setStartDelay(long milliseconds) { delay = Math.max(0, milliseconds); }
    @Override public long getStartDelay() { return delay; }
    @Override public void setInterpolator(TimeInterpolator value) { interpolator = value; }
    @Override public TimeInterpolator getInterpolator() { return interpolator; }
    @Override public boolean isStarted() { return started; }
    @Override public boolean isRunning() { return running; }

    @Override public void start() {
        if (started) return;
        started = true;
        if (delay == 0) doFrame(0);
        else Choreographer.getInstance().postFrameCallbackDelayed(this, delay);
    }
    private void begin() {
        if (running) return;
        running = true;
        for (AnimatorListener listener : listeners()) listener.onAnimationStart(this);
    }
    private void configure(Animator child) {
        if (duration >= 0) child.setDuration(duration);
        if (interpolator != null) child.setInterpolator(interpolator);
    }
    @Override public void doFrame(long nanos) {
        if (!started) return;
        if (!running) {
            begin();
            if (!started) return;
            for (Animator child : getChildAnimations()) {
                configure(child);
                child.start();
                if (!started) return;
            }
        }
        for (Animator child : children) {
            if (child.isStarted()) {
                Choreographer.getInstance().postFrameCallback(this);
                return;
            }
        }
        finish();
    }
    @Override public void cancel() {
        if (!started) return;
        begin();
        for (AnimatorListener listener : listeners()) listener.onAnimationCancel(this);
        for (Animator child : getChildAnimations()) child.cancel();
        finish();
    }
    @Override public void end() {
        started = true;
        begin();
        for (Animator child : getChildAnimations()) {
            configure(child);
            child.end();
        }
        finish();
    }
    private void finish() {
        Choreographer.getInstance().removeFrameCallback(this);
        running = started = false;
        for (AnimatorListener listener : listeners()) listener.onAnimationEnd(this);
    }
}
