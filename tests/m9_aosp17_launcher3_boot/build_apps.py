#!/usr/bin/env python3
"""Build small, executable APKs with localized labels and adaptive vector icons."""

import argparse
from pathlib import Path
import subprocess
import tempfile
from xml.sax.saxutils import escape
import zipfile

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


def build_app(args, stage, dex, name, app):
    english, chinese, background, foreground, silhouette, details, accent, category = app
    optional_filter = f"""
        <intent-filter>
            <action android:name="android.intent.action.MAIN"/>
            <category android:name="android.intent.category.{category}"/>
            <category android:name="android.intent.category.DEFAULT"/>
        </intent-filter>""" if category else ""
    manifest = write(stage, "AndroidManifest.xml", f"""<manifest xmlns:android="{NS}"
        package="org.wasmandroid.fixtures.{name}" android:versionCode="1">
    <application android:label="@string/app_name" android:icon="@drawable/icon">
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
    write(stage, "res/drawable/icon.xml", f"""<adaptive-icon xmlns:android="{NS}">
    <background android:drawable="@color/icon_background"/>
    <foreground android:drawable="@drawable/icon_foreground"/>
</adaptive-icon>""")
    write(stage, "res/drawable/icon_foreground.xml", f"""<vector xmlns:android="{NS}"
    android:width="108dp" android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">
    <group android:translateX="22" android:translateY="22">
      <path android:fillColor="{foreground}" android:pathData="{silhouette}"/>
      <path android:fillColor="{accent}" android:pathData="{details}"/>
    </group>
</vector>""")
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
