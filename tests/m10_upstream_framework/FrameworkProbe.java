package org.wasmandroid.test;

import android.content.IntentFilter;
import android.content.res.XmlResourceParser;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Matrix;
import android.graphics.Path;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;

import com.android.internal.R;
import com.android.server.IntentResolver;

import java.util.ArrayList;

public final class FrameworkProbe {
    public static boolean checkLayoutAttributes(XmlResourceParser parser) throws Exception {
        String android = "http://schemas.android.com/apk/res/android";
        if (parser.getEventType() != XmlResourceParser.START_TAG || parser.getDepth() != 1
                || !"TextView".equals(parser.getName())
                || parser.getAttributeResourceValue(android, "id", 0) != R.id.text1
                || parser.getAttributeIntValue(android, "layout_width", 0) != -1
                || parser.getAttributeIntValue(android, "layout_height", 0) != -2) {
            return false;
        }
        int width = R.styleable.ViewGroup_Layout[R.styleable.ViewGroup_Layout_layout_width];
        for (int i = 0; i < parser.getAttributeCount(); i++) {
            if (parser.getAttributeNameResource(i) == width) {
                return "layout_width".equals(parser.getAttributeName(i));
            }
        }
        return false;
    }

    public static void drawPrimitives(Canvas canvas, Paint paint) {
        canvas.drawRoundRect(1, 2, 14, 15, 2, 3, paint);
        paint.setTextSize(8);
        canvas.drawText("AOSP", 2, 10, paint);
        int[] colors = {0, 0xff123456, 0x80326394, 0, 0xff654321, 0xff102030};
        canvas.drawBitmap(colors, 1, 3, 8f, 9f, 2, 2, true, null);
    }

    private static final class Receiver implements Handler.Callback {
        private int delivered;

        @Override
        public boolean handleMessage(Message message) {
            delivered = 10 * delivered + message.what;
            if (message.what == 4) {
                Looper.myLooper().quitSafely();
            }
            return true;
        }
    }

    public static int dispatchMessages() {
        Looper.prepare();
        Receiver receiver = new Receiver();
        Handler handler = new Handler(Looper.myLooper(), receiver);
        handler.sendEmptyMessageAtTime(3, 20);
        handler.sendEmptyMessageAtTime(1, 10);
        handler.sendEmptyMessageAtTime(4, 20);
        handler.sendEmptyMessageAtTime(7, 1000);
        handler.sendMessageAtFrontOfQueue(handler.obtainMessage(2));
        Looper.loop();
        return receiver.delivered;
    }

    public static Path translatedPath() {
        Matrix transform = new Matrix();
        transform.setTranslate(5, 7);
        float[] vertices = {1, 2, 3, 2, 3, 6};
        transform.mapPoints(vertices);
        Path path = new Path();
        path.moveTo(vertices[0], vertices[1]);
        path.lineTo(vertices[2], vertices[3]);
        path.lineTo(vertices[4], vertices[5]);
        path.close();
        return path;
    }

    private static final class Resolver extends IntentResolver<IntentFilter, IntentFilter> {
        @Override
        protected IntentFilter getIntentFilter(IntentFilter filter) {
            return filter;
        }

        @Override
        protected IntentFilter[] newArray(int size) {
            return new IntentFilter[size];
        }

        @Override
        protected boolean isPackageForFilter(String name, IntentFilter filter) {
            return false;
        }
    }

    public static int indexFilters() throws IntentFilter.MalformedMimeTypeException {
        Resolver resolver = new Resolver();
        IntentFilter image = new IntentFilter("org.example.OPEN", "image/*");
        IntentFilter text = new IntentFilter("org.example.OPEN", "text/plain");
        resolver.addFilter(null, image);
        resolver.addFilter(null, text);
        IntentFilter query = new IntentFilter("org.example.OPEN", "image/*");
        ArrayList<IntentFilter> found = resolver.findFilters(query);
        if (found == null || found.size() != 1 || found.get(0) != image) {
            return 1;
        }
        if (image.match("org.example.OPEN", "image/png", null, null, null, "test") < 0
                || image.match("org.example.OPEN", "text/plain", null, null, null, "test")
                    != IntentFilter.NO_MATCH_TYPE) {
            return 2;
        }
        resolver.removeFilter(image);
        return resolver.findFilters(query) == null ? 0 : 3;
    }
}
