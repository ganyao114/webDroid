#!/usr/bin/env python3
"""Build small APKs with localized labels and adaptive, vector and PNG icons."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from xml.sax.saxutils import escape
import zipfile
import zlib

NS = "http://schemas.android.com/apk/res/android"
ACTIVITY = "org.wasmandroid.fixtures.DemoActivity"
APPS = {
    "settings": ("Settings", "设置", "#FF366990", "#FFD9ECFC",
                 "M21,6h22v8H21z M25,16h14v32H25z M13,25h38v14H13z M21,50h22v8H21z",
                 "M32,24a8,8 0,1 0,0 16a8,8 0,1 0,0 -16z", "#FF366990", None),
    "browser": ("Browser", "浏览器", "#FFCCEBDC", "#FF1A7866",
                "M32,11a21,21 0,1 0,0 42a21,21 0,1 0,0 -42z",
                "M23,41l5,-13l13,-5l-5,13z", "#FFFFFFFF", "APP_BROWSER"),
    "gallery": ("Gallery", "图库", "#FF725296", "#FFF4DCFF",
                "M14,17h36v30H14z", "M17,44l11,-13l7,8l5,-5l7,10z M38,23h7v7h-7z",
                "#FF725296", "APP_GALLERY"),
    "calculator": ("Calculator", "计算器", "#FFFFE4BB", "#FF9C5200",
                   "M19,12h26v40H19z",
                   "M23,17h18v8H23z M23,30h7v6h-7z M34,30h7v6h-7z M23,40h7v6h-7z M34,40h7v6h-7z",
                   "#FFFFE4BB", None),
    "clock": ("Clock", "时钟", "#FFDAE2FF", "#FF344DA0",
              "M32,11a21,21 0,1 0,0 42a21,21 0,1 0,0 -42z",
              "M29,18h5v13l9,6l-3,4l-11,-8z", "#FFFFFFFF", None),
}


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def write(directory, name, text):
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def write_settings_icon(stage, background, foreground):
    # Draw the fixture's settings glyph at its xhdpi resource size.
    size = 108
    background, foreground = (bytes.fromhex(color[3:]) for color in (background, foreground))
    rectangles = [(43, 28, 65, 36), (47, 38, 61, 70), (35, 47, 73, 61), (43, 72, 65, 80)]
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # PNG filter None
        for x in range(size):
            glyph = any(left <= x < right and top <= y < bottom
                        for left, top, right, bottom in rectangles)
            hole = (x - 54) ** 2 + (y - 54) ** 2 < 8 ** 2
            rows.extend(foreground if glyph and not hole else background)

    def chunk(kind, data):
        checksum = zlib.crc32(kind + data)
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

    path = stage / "res/mipmap-xhdpi/icon.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def build_app(args, stage, dex, name, app):
    english, chinese, background, foreground, silhouette, details, accent, category = app
    optional_filter = f"""
        <intent-filter>
            <action android:name="android.intent.action.MAIN"/>
            <category android:name="android.intent.category.{category}"/>
            <category android:name="android.intent.category.DEFAULT"/>
        </intent-filter>""" if category else ""
    icon_type = "mipmap" if name in ("browser", "settings") else "drawable"
    manifest = write(stage, "AndroidManifest.xml", f"""<manifest xmlns:android="{NS}"
        package="org.wasmandroid.fixtures.{name}" android:versionCode="1">
    <application android:label="@string/app_name" android:icon="@{icon_type}/icon">
      <activity android:name="{ACTIVITY}" android:exported="true">
        <intent-filter>
          <action android:name="android.intent.action.MAIN"/>
          <category android:name="android.intent.category.LAUNCHER"/>
        </intent-filter>
        {optional_filter}
      </activity>
    </application>
</manifest>""")
    for directory, label in (("values", english), ("values-en-rUS", english), ("values-zh-rCN", chinese)):
        write(stage, f"res/{directory}/strings.xml", f"""<resources>
    <string name="app_name">{escape(label)}</string>
    <color name="icon_background">{background}</color>
</resources>""")
    if name == "settings":
        write_settings_icon(stage, background, foreground)
    else:
        standalone_vector = name == "calculator"
        vector_name = "icon" if standalone_vector else "icon_foreground"
        background_path = (f'<path android:fillColor="{background}" android:pathData="M0,0h108v108H0z"/>'
                           if standalone_vector else "")
        write(stage, f"res/drawable/{vector_name}.xml", f"""<vector xmlns:android="{NS}"
    android:width="108dp" android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">
    {background_path}
    <group android:translateX="22" android:translateY="22">
      <path android:fillColor="{foreground}" android:pathData="{silhouette}"/>
      <path android:fillColor="{accent}" android:pathData="{details}"/>
    </group>
</vector>""")
        if not standalone_vector:
            background_ref = "@android:color/white" if name == "browser" else "@color/icon_background"
            directory = "mipmap-anydpi-v26" if icon_type == "mipmap" else "drawable"
            write(stage, f"res/{directory}/icon.xml", f"""<adaptive-icon xmlns:android="{NS}">
    <background android:drawable="{background_ref}"/>
    <foreground android:drawable="@drawable/icon_foreground"/>
</adaptive-icon>""")
    compiled = stage / "resources.zip"
    apk = stage / f"{name}.apk"
    run(args.aapt2, "compile", "--dir", stage / "res", "-o", compiled)
    run(args.aapt2, "link", "-I", args.android_jar, "--manifest", manifest,
        "--min-sdk-version", 26, "--target-sdk-version", 36, "-o", apk, compiled)
    with zipfile.ZipFile(apk, "a") as archive:
        archive.write(dex, "classes.dex")
    apk.replace(args.out / apk.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("out", "aapt2", "android-jar", "javac", "d8"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out) as directory:
        stage = Path(directory)
        java = write(stage, "DemoActivity.java", """package org.wasmandroid.fixtures;

public final class DemoActivity extends android.app.Activity {
    @Override protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        android.widget.TextView content = new android.widget.TextView(this);
        content.setText(getApplicationInfo().loadLabel(getPackageManager()));
        setContentView(content);
    }
}
""")
        classes = stage / "classes"
        classes.mkdir()
        run(args.javac, "--release", 8, "-classpath", args.android_jar, "-d", classes, java)
        run(args.d8, "--release", "--min-api", 26, "--lib", args.android_jar, "--output", stage,
            classes / "org/wasmandroid/fixtures/DemoActivity.class")
        for name, app in APPS.items():
            build_app(args, stage, stage / "classes.dex", name, app)


if __name__ == "__main__":
    main()
