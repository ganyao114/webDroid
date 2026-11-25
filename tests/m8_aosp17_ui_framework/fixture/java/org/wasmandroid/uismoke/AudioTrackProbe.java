package org.wasmandroid.uismoke;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class AudioTrackProbe {
    private static void check(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }

    public static void verify() {
        int size = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        check(size > 882 && AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) == 48000,
                "AudioTrack reports the PCM sink format and a usable buffer size");
        AudioTrack mono = new AudioTrack(AudioManager.STREAM_MUSIC, 44100, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT, size, AudioTrack.MODE_STREAM);
        try {
            check(mono.getState() == AudioTrack.STATE_INITIALIZED && mono.getAudioSessionId() > 0,
                    "Legacy constructor initializes a mono PCM track");
            mono.setStereoVolume(0.5f, 1f);
            short[] samples = new short[439];
            for (int i = 0; i < samples.length; i++) samples[i] = 16384;
            check(mono.write(samples, 0, samples.length) == 439, "Queue shorts before play");
            mono.play();
            check(mono.write(new byte[]{0, 64}, 0, 2, AudioTrack.WRITE_NON_BLOCKING) == 2,
                    "Write a little-endian PCM frame");
            ByteBuffer data = ByteBuffer.allocateDirect(6).order(ByteOrder.LITTLE_ENDIAN);
            data.putShort(2, (short) 16384);
            data.position(2);
            data.limit(4);
            ByteBuffer slice = data.slice().asReadOnlyBuffer();
            check(mono.write(slice, 2, AudioTrack.WRITE_NON_BLOCKING) == 2 && slice.position() == 2,
                    "Write a sliced direct buffer and advance its position");
            mono.pause();
            mono.flush();
            check(mono.getPlaybackHeadPosition() == 0, "Flush resets the paused playback head");
        } finally {
            mono.release();
        }
        check(mono.getState() == AudioTrack.STATE_UNINITIALIZED, "Release clears the track state");

        AudioFormat format = new AudioFormat.Builder().setSampleRate(48000)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).build();
        AudioTrack stereo = new AudioTrack(new AudioAttributes.Builder().build(), format, 16, AudioTrack.MODE_STATIC, 0);
        try {
            check(stereo.getState() == AudioTrack.STATE_NO_STATIC_DATA, "Static track waits for data");
            check(stereo.write(new float[]{0.75f, -0.25f, 0.5f, -0.5f}, 0, 4,
                    AudioTrack.WRITE_BLOCKING) == 4, "Static track accepts stereo float samples");
            stereo.play();
        } finally {
            stereo.release();
        }
    }
}
