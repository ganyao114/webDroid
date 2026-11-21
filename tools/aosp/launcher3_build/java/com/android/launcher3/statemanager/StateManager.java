package com.android.launcher3.statemanager;

import android.animation.Animator;
import com.android.launcher3.Launcher;
import com.android.launcher3.LauncherState;

public final class StateManager<S extends LauncherState, A extends Launcher> {
    private final A activity;
    private S state;
    private S lastState;

    public StateManager(A activity, S initialState) {
        this.activity = activity;
        state = lastState = initialState;
    }

    public S getLastState() { return lastState; }

    public void goToState(S target, Animator.AnimatorListener listener) {
        if (state != target) {
            state.onLeavingState(activity, target);
            lastState = state;
            state = target;
        }
        activity.applyState(target, listener);
    }

    public void onBackStarted(S target) { activity.setBackProgress(0); }
    public void onBackProgressed(S target, float progress) { activity.setBackProgress(progress); }
    public void onBackCancelled(S target) { activity.setBackProgress(0); }
}
