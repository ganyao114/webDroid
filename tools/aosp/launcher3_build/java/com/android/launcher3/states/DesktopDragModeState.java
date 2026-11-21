package com.android.launcher3.states;

import com.android.launcher3.LauncherState;
import com.android.launcher3.logging.StatsLogManager;
import com.android.launcher3.views.ActivityContext;

public final class DesktopDragModeState extends LauncherState {
    public DesktopDragModeState(int id) {
        super(id, StatsLogManager.LAUNCHER_STATE_HOME, EditModeState.EDIT_FLAGS);
    }

    @Override public int getTransitionDuration(ActivityContext context, boolean entering) { return 150; }
}
