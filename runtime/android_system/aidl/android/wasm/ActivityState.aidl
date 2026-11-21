package android.wasm;

parcelable ActivityState {
    @utf8InCpp String packageName;
    @utf8InCpp String activityName;
    int taskId;
    @utf8InCpp String state;
    int launchedCount;
}
