package java.util.concurrent;

public enum TimeUnit {
    NANOSECONDS(1), MICROSECONDS(1_000), MILLISECONDS(1_000_000), SECONDS(1_000_000_000),
    MINUTES(60_000_000_000L), HOURS(3_600_000_000_000L), DAYS(86_400_000_000_000L);

    private final long nanos;
    TimeUnit(long nanos) { this.nanos = nanos; }

    public long convert(long value, TimeUnit unit) {
        if (nanos >= unit.nanos) return value / (nanos / unit.nanos);
        long scale = unit.nanos / nanos;
        if (value > Long.MAX_VALUE / scale) return Long.MAX_VALUE;
        if (value < Long.MIN_VALUE / scale) return Long.MIN_VALUE;
        return value * scale;
    }
    public long toNanos(long value) { return NANOSECONDS.convert(value, this); }
    public long toMicros(long value) { return MICROSECONDS.convert(value, this); }
    public long toMillis(long value) { return MILLISECONDS.convert(value, this); }
    public long toSeconds(long value) { return SECONDS.convert(value, this); }
    public long toMinutes(long value) { return MINUTES.convert(value, this); }
    public long toHours(long value) { return HOURS.convert(value, this); }
    public long toDays(long value) { return DAYS.convert(value, this); }
}
