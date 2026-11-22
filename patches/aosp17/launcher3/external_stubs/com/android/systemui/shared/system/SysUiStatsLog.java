package com.android.systemui.shared.system;

/** Compile-time constants for the Launcher atom's user type field. */
public interface SysUiStatsLog {
    int LAUNCHER_UICHANGED__USER_TYPE__TYPE_UNKNOWN = 0;
    int LAUNCHER_UICHANGED__USER_TYPE__TYPE_MAIN = 1;
    int LAUNCHER_UICHANGED__USER_TYPE__TYPE_WORK = 2;
    int LAUNCHER_UICHANGED__USER_TYPE__TYPE_PRIVATE = 3;
    int LAUNCHER_UICHANGED__USER_TYPE__TYPE_CLONED = 4;
}
