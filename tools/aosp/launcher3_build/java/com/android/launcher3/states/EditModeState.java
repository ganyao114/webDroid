package com.android.launcher3.states;

import com.android.launcher3.LauncherState;
import com.android.launcher3.logging.StatsLogManager;
import com.android.launcher3.views.ActivityContext;

/** Supplies the state metadata used by the upstream LauncherState table. */
public class EditModeState extends LauncherState {
    static final int EDIT_FLAGS = FLAG_MULTI_PAGE | FLAG_WORKSPACE_INACCESSIBLE
            | FLAG_DISABLE_RESTORE_EXCEPT_UI_MODE_CHANGE | FLAG_WORKSPACE_ICONS_CAN_BE_DRAGGED
            | FLAG_WORKSPACE_HAS_BACKGROUNDS | FLAG_WORKSPACE_ICONS_BEING_DRAGGED;

    public EditModeState(int id) {
        super(id, StatsLogManager.LAUNCHER_STATE_HOME, EDIT_FLAGS);
    }

    @Override public int getTransitionDuration(ActivityContext context, boolean entering) { return 150; }
    @Override protected float getDepthUnchecked(ActivityContext context) { return 0.15f; }
}
