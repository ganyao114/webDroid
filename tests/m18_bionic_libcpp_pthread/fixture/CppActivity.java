package com.wasmandroid.cpppthread;

import android.app.Activity;
import android.os.Bundle;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class CppActivity extends Activity {
    private LinearLayout root;
    private int callbacks;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(20, 20, 20, 20);
        setContentView(root);
        System.loadLibrary("cpp_pthread_jni");
        if (runChecks() != 0 || callbacks != 1)
            throw new AssertionError("NDK checks or Java callback failed");
    }

    private native int runChecks();

    private void onNativeReport(String containers, String exception, String threads, String runtime) {
        callbacks++;
        for (String text : new String[]{containers, exception, threads, runtime}) {
            TextView badge = new TextView(this);
            badge.setText(text);
            badge.setTextSize(20);
            badge.setPadding(0, 4, 0, 4);
            root.addView(badge);
        }
    }
}
