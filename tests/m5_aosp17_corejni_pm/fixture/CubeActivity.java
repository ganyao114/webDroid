package org.wasmandroid.cube;

import android.app.Activity;
import android.os.Bundle;

public final class CubeActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        System.loadLibrary("ndk_gles3_cube");
        setContentView(getResources().getIdentifier("activity_main", "layout", getPackageName()));
    }
}
