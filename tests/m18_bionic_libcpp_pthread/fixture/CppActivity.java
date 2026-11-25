package com.wasmandroid.cpppthread;

import android.app.Activity;
import android.os.Bundle;
import android.os.Debug;
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
        if (runChecks() != 0 || callbacks != 1 || beginReferenceTest() != 0 || finishReferenceTest() != 0)
            throw new AssertionError("NDK checks or Java callback failed");
        Runtime runtime = Runtime.getRuntime();
        long total = runtime.totalMemory(), free = runtime.freeMemory();
        long reserved = Debug.getNativeHeapSize(), allocated = Debug.getNativeHeapAllocatedSize();
        if (total <= 0 || free < 0 || free > total || total > runtime.maxMemory()
                || allocated <= 0 || reserved != allocated + Debug.getNativeHeapFreeSize()) {
            throw new AssertionError("Invalid managed or native heap statistics");
        }
    }

    private native int runChecks();
    private native int beginReferenceTest();
    private native int finishReferenceTest();

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
