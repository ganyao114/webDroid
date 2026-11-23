package com.wasmandroid.hybrid;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.nio.ByteBuffer;

public final class HybridActivity extends Activity {
    private static final int INK = 0xff14243b;
    private static final int MUTED = 0xff45617c;
    private static final int BLUE = 0xff2455c3;
    private static final int TEAL = 0xff127a72;
    private TextView checksum, score, status, callbackCount;
    private ImageView swatch;
    private ByteBuffer bytes;
    private int seed, callbacks;
    private int nativeVisits;
    private double nativeScore;
    private long nativeChecksum;
    private int histogram;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        bytes = ByteBuffer.allocateDirect(8);
        bytes.put(0, (byte) 0x48);

        System.loadLibrary("hybrid_jni");
        Runtime.getRuntime().loadLibrary("hybrid_jni");
        String abi = HybridBridge.abi();
        String directPath = getApplicationInfo().sourceDir + "!/lib/" + abi + "/libhybrid_jni.so";
        System.load(directPath);
        Runtime.getRuntime().load(directPath);
        check(HybridBridge.loadCount() == 1, "Repeated library loading");
        try {
            System.loadLibrary("hybrid_absent");
            throw new AssertionError("Missing library unexpectedly loaded");
        } catch (UnsatisfiedLinkError expected) {
            // Missing APK entries must complete through the Java exception path.
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xffedf3fa);
        root.setPadding(24, 24, 24, 24);
        TextView title = text(root, "Hybrid JNI", 32, INK);
        title.setPadding(0, 0, 0, 8);
        text(root, abi.equals("arm64-v8a") ? "ARM64  |  arm64-v8a" : "ARMv7  |  armeabi-v7a", 16, BLUE);
        text(root, "DT_NEEDED + .init_array OK", 16, TEAL);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(0xffffffff);
        card.setPadding(18, 10, 18, 16);
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(-1, -2);
        cardParams.topMargin = 18;
        cardParams.bottomMargin = 12;
        root.addView(card, cardParams);
        checksum = text(card, "", 22, INK);
        score = text(card, "", 17, MUTED);
        callbackCount = text(card, "", 17, MUTED);
        swatch = new ImageView(this);
        card.addView(swatch, new LinearLayout.LayoutParams(64, 12));

        String exception;
        try {
            HybridBridge.requirePositive(-4);
            throw new AssertionError("Expected native exception");
        } catch (IllegalArgumentException caught) {
            check("Input must be positive".equals(caught.getMessage()), "JNI exception message");
            exception = "CAUGHT_IAE";
        }
        text(root, exception, 16, TEAL);
        status = text(root, "", 16, TEAL);
        text(root, "Re-entry OK", 16, TEAL);

        check(HybridBridge.score(2.75, 1.25, 2.0) == 19.25, "double arguments and return");
        check(HybridBridge.scale(1.25f, 3) == 3.75f, "float return");
        check(HybridBridge.wide(1, 2, 3, 4, 5, 6, 7, 8, 9, 0x700000002L,
                0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 0.25f) == 484.75, "JNI incoming stack arguments");
        check(HybridBridge.verifyAbi(), "JNI callback ABI");
        check(HybridBridge.decorated_name(new int[]{4, 7}) == 11, "Array overload");
        check(HybridBridge.decorated_name("J\u0000\u00e9") == 3, "String overload");
        check(HybridBridge.métier() == 17, "Unicode JNI name");
        text(root, "JNI ABI OK", 16, TEAL);

        Button compute = new Button(this);
        compute.setAllCaps(false);
        compute.setText("Run Native Compute (+1)");
        compute.setTextSize(17);
        compute.setTextColor(0xffffffff);
        compute.setBackgroundColor(BLUE);
        root.addView(compute, new LinearLayout.LayoutParams(-1, 52));
        compute.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) { compute(++seed); }
        });
        setContentView(root);
        compute(0);
    }

    private TextView text(LinearLayout column, String value, float size, int color) {
        TextView label = new TextView(this);
        label.setText(value);
        label.setTextSize(size);
        label.setTextColor(color);
        label.setPadding(0, 5, 0, 5);
        column.addView(label, new LinearLayout.LayoutParams(-1, -2));
        return label;
    }

    private void compute(int input) {
        byte[] scratch = new byte[]{2, 4, 6};
        ByteBuffer view = bytes.duplicate();
        view.position(1);
        ByteBuffer slice = view.slice();
        check(HybridBridge.run(this, input, scratch, slice) == 37, "Guest to Java to guest");
        check(callbacks == input + 1 && nativeVisits == callbacks && histogram == input + 16, "Callback state");
        check(scratch[0] == 6 && scratch[1] == 4 && scratch[2] == 9, "JNI array release modes");
        check(nativeChecksum == 4294967297L + input * 37, "Native checksum");
        check(nativeScore == 19.25 + input * 2, "Native double");
        check(bytes.get(0) == 0x48 && bytes.get(1) == 0x59 && bytes.get(2) == (byte) (0x80 + input),
                "Java direct buffer alias");
        check(HybridBridge.lastLong == nativeChecksum && HybridBridge.lastDouble == nativeScore
                && HybridBridge.lastFloat == 1.25f && HybridBridge.lastByte == -7, "JNI static fields");
        ByteBuffer fromNative = HybridBridge.nativeBuffer();
        check(fromNative.isDirect() && !fromNative.hasArray() && fromNative.get(0) == 0x21, "JNI direct buffer");
        fromNative.put(1, (byte) 0x67);
        check(HybridBridge.readNativeBuffer() == 0x67, "Java write to guest memory");

        String banner = HybridBridge.dynamicBanner("Checksum", input);
        check(banner.equals("Checksum " + nativeChecksum), "JNI dynamic String");
        checksum.setText(banner);
        score.setText("Double score " + nativeScore);
        callbackCount.setText("Callbacks " + callbacks);
        status.setText("Direct buffer OK");
        int[] pixels = new int[32 * 8];
        java.util.Arrays.fill(pixels, (input & 1) == 0 ? TEAL : BLUE);
        swatch.setImageBitmap(Bitmap.createBitmap(pixels, 32, 8, Bitmap.Config.ARGB_8888));
    }

    private void onNativeReport(String title, long checksum, double score, int[] bins) {
        check("Native report".equals(title) && bins.length == 4, "JNI callback arguments");
        check(bytes.get(1) == 0x59, "Direct buffer write visible during native callback");
        nativeChecksum = checksum;
        nativeScore = score;
        histogram = bins[3];
        callbacks++;
    }

    private static void check(boolean ok, String operation) {
        if (!ok) throw new AssertionError(operation);
    }
}
