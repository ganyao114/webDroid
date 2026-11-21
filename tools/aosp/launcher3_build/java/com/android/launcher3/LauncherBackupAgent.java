package com.android.launcher3;

import android.app.backup.BackupAgentHelper;
import android.app.backup.SharedPreferencesBackupHelper;

public final class LauncherBackupAgent extends BackupAgentHelper {
    @Override
    public void onCreate() {
        addHelper("launcher_preferences", new SharedPreferencesBackupHelper(this,
                LauncherFiles.SHARED_PREFERENCES_KEY));
    }
}
