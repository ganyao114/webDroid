package android.database.sqlite;

public final class SQLiteStatement implements AutoCloseable {
    private final SQLiteDatabase database;
    private final String sql;
    private String[] bindings;
    private boolean closed;

    SQLiteStatement(SQLiteDatabase database, String sql) {
        this.database = database;
        this.sql = java.util.Objects.requireNonNull(sql);
    }

    public void bindAllArgsAsStrings(String[] values) {
        if (closed) throw new IllegalStateException("Statement is closed");
        bindings = values == null ? null : values.clone();
    }

    public long simpleQueryForLong() {
        if (closed) throw new IllegalStateException("Statement is closed");
        return database.queryLong(sql, bindings);
    }

    @Override public void close() { closed = true; bindings = null; }
}
