package org.wasmandroid.cube;

import android.app.Activity;
import android.os.Bundle;

public final class CubeActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(R.layout.activity_main);
    }

    public static int surfaceWidth() { return Wasm.surfaceWidth(); }
    public static int surfaceHeight() { return Wasm.surfaceHeight(); }
    public static float frameTimeSec(int frameIdx) { return Wasm.frameTimeSec(frameIdx); }
    public static int expectedAudioFrames() { return Wasm.expectedAudioFrames(); }

    // Primitive static code for ART's standalone graph builder.
    public static final class Wasm {
        public static int surfaceWidth() { return 256; }
        public static int surfaceHeight() { return 256; }
        public static float frameTimeSec(int frameIdx) { return frameIdx / 10.0f; }

        public static int expectedAudioFrames() {
            // Debug DEX keeps the local arithmetic and branch for ART's optimizer.
            int blocks = 14;
            int blockSize = 10;
            int frames = blocks * blockSize;
            if (frames == 140) {
                return frames;
            }
            return -1;
        }
    }
}
