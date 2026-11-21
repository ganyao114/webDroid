package android.wasm;

import android.wasm.ActivityState;

interface IActivityManagerLite {
    ActivityState startActivity(in @utf8InCpp String packageName, in @utf8InCpp String activityName);
    ActivityState startHomeActivity();
    ActivityState getTopActivity();
    int finishTopActivity();
}
