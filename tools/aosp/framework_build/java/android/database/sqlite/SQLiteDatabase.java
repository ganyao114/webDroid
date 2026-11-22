package android.database.sqlite;

import android.content.ContentValues;
import android.database.Cursor;
import android.database.DatabaseErrorHandler;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

/** In-memory storage for the embedded launcher database profile. */
public final class SQLiteDatabase implements AutoCloseable {
    // This store keeps live tables and has no separate SQLite pager cache.
    public static int releaseMemory() { return 0; }
    public static final int CONFLICT_REPLACE = 5;
    public static final int OPEN_READONLY = 1, NO_LOCALIZED_COLLATORS = 16;
    public static final int CREATE_IF_NECESSARY = 0x10000000, ENABLE_LEGACY_COMPATIBILITY_WAL = 0x80000000;
    private static final Map<String, Data> databases = new LinkedHashMap<>();

    public interface CursorFactory {}
    public static final class OpenParams {
        private final int flags;
        private final boolean wal;
        private OpenParams(Builder builder) { flags = builder.flags; wal = builder.wal; }
        public Builder toBuilder() { return new Builder(this); }

        public static final class Builder {
            private int flags;
            private boolean wal;
            public Builder() {}
            public Builder(OpenParams params) { flags = params.flags; wal = params.wal; }
            public Builder addOpenFlags(int flags) { this.flags |= flags; return this; }
            public Builder removeOpenFlags(int flags) { this.flags &= ~flags; return this; }
            public boolean isWriteAheadLoggingEnabled() { return wal; }
            public Builder setWriteAheadLoggingEnabled(boolean enabled) { wal = enabled; return this; }
            public Builder setCursorFactory(CursorFactory factory) { return this; }
            public Builder setErrorHandler(DatabaseErrorHandler handler) { return this; }
            public Builder setLookasideConfig(int size, int count) {
                if (size < 0 || count < 0) throw new IllegalArgumentException();
                return this;
            }
            public Builder setIdleConnectionTimeout(long time) {
                if (time < 0) throw new IllegalArgumentException();
                return this;
            }
            public OpenParams build() { return new OpenParams(this); }
        }
    }

    private static final class Table {
        final ArrayList<String> columns = new ArrayList<>();
        final ArrayList<String> keys = new ArrayList<>();
        final ArrayList<ContentValues> rows = new ArrayList<>();
        final ContentValues defaults = new ContentValues();
        long nextId = 1;
        Table copy() {
            Table copy = new Table();
            copy.columns.addAll(columns);
            copy.keys.addAll(keys);
            copy.defaults.putAll(defaults);
            for (ContentValues row : rows) copy.rows.add(new ContentValues(row));
            copy.nextId = nextId;
            return copy;
        }
    }

    private static final class Data {
        int version;
        Map<String, Table> tables = new LinkedHashMap<>();
        Data copy() {
            Data copy = new Data();
            copy.version = version;
            for (Map.Entry<String, Table> entry : tables.entrySet()) {
                copy.tables.put(entry.getKey(), entry.getValue().copy());
            }
            return copy;
        }
    }

    private final Data data;
    private boolean open = true;
    private boolean readOnly;
    private Data snapshot;
    private boolean successful;

    private SQLiteDatabase(Data data, OpenParams params) {
        this.data = data;
        readOnly = (params.flags & OPEN_READONLY) != 0;
    }

    public static SQLiteDatabase createInMemory(OpenParams params) {
        return new SQLiteDatabase(new Data(), params);
    }
    public static SQLiteDatabase openDatabase(File file, OpenParams params) {
        String path = file.getPath();
        Data data = databases.get(path);
        if (data == null) {
            if ((params.flags & CREATE_IF_NECESSARY) == 0 || (params.flags & OPEN_READONLY) != 0) {
                throw new SQLiteException("No database at " + path);
            }
            data = new Data();
            databases.put(path, data);
        }
        return new SQLiteDatabase(data, params);
    }
    public static boolean deleteDatabase(File file) { return databases.remove(file.getPath()) != null; }
    public String getPath() { return ":memory:"; }
    public boolean isOpen() { return open; }
    public boolean isReadOnly() { return readOnly; }
    void reopenReadWrite() { readOnly = false; }
    public int getVersion() { checkOpen(); return data.version; }
    public void setVersion(int version) { checkWritable(); data.version = version; }
    public boolean enableWriteAheadLogging() { return false; }
    public void disableWriteAheadLogging() {}
    @Override public void close() { if (inTransaction()) endTransaction(); open = false; }
    public boolean inTransaction() { return snapshot != null; }
    public void beginTransaction() {
        checkWritable();
        if (snapshot != null) throw new IllegalStateException("Nested database transaction");
        snapshot = data.copy();
        successful = false;
    }
    public void setTransactionSuccessful() {
        if (snapshot == null) throw new IllegalStateException("No database transaction");
        successful = true;
    }
    public void endTransaction() {
        if (snapshot == null) throw new IllegalStateException("No database transaction");
        if (!successful) {
            data.version = snapshot.version;
            data.tables = snapshot.tables;
        }
        snapshot = null;
    }
    private void checkOpen() {
        if (!open) throw new IllegalStateException("Database is closed");
    }
    private void checkWritable() {
        checkOpen();
        if (readOnly) throw new SQLiteException("Database is read-only");
    }
    private Table table(String name) {
        checkOpen();
        Table table = data.tables.get(name);
        if (table == null) throw new SQLiteException("No such table " + name);
        return table;
    }

    public void execSQL(String sql) {
        checkWritable();
        String trimmed = sql.trim();
        if (trimmed.endsWith(";")) trimmed = trimmed.substring(0, trimmed.length() - 1).trim();
        String upper = trimmed.toUpperCase(Locale.ROOT);
        if (upper.startsWith("DROP TABLE IF EXISTS ")) {
            data.tables.remove(trimmed.substring(21).trim());
            return;
        }
        if (!upper.startsWith("CREATE TABLE ")) throw new SQLiteException("Unsupported SQL " + sql);
        int body = trimmed.indexOf('(');
        int end = trimmed.lastIndexOf(')');
        if (body < 0 || end < body) throw new SQLiteException("Invalid CREATE TABLE");
        String name = trimmed.substring(13, body).trim();
        boolean optional = name.toUpperCase(Locale.ROOT).startsWith("IF NOT EXISTS ");
        if (optional) name = name.substring(14).trim();
        if (data.tables.containsKey(name)) {
            if (optional) return;
            throw new SQLiteException("Table already exists " + name);
        }
        Table table = new Table();
        int start = body + 1, depth = 0;
        for (int i = start; i <= end; ++i) {
            char ch = trimmed.charAt(i);
            if (i == end || ch == ',' && depth == 0) {
                String definition = trimmed.substring(start, i).trim();
                addColumn(table, definition);
                start = i + 1;
            } else if (ch == '(') depth++;
            else if (ch == ')') depth--;
        }
        data.tables.put(name, table);
    }

    private static void addColumn(Table table, String definition) {
        String upper = definition.toUpperCase(Locale.ROOT);
        if (upper.startsWith("PRIMARY KEY")) {
            String keys = definition.substring(definition.indexOf('(') + 1, definition.lastIndexOf(')'));
            for (String key : keys.split(",")) table.keys.add(key.trim());
            return;
        }
        int space = definition.indexOf(' ');
        if (space < 0) throw new SQLiteException("Invalid column " + definition);
        String name = definition.substring(0, space);
        table.columns.add(name);
        if (upper.contains(" PRIMARY KEY")) table.keys.add(name);
        int defaultAt = upper.indexOf(" DEFAULT ");
        if (defaultAt >= 0) {
            String expression = definition.substring(defaultAt + 9).trim();
            if (expression.startsWith("'") && expression.endsWith("'")) {
                table.defaults.put(name, expression.substring(1, expression.length() - 1));
            } else if (!"NULL".equalsIgnoreCase(expression)) {
                table.defaults.put(name, Long.parseLong(expression));
            }
        }
    }

    public long insert(String name, String nullColumnHack, ContentValues values) {
        return insertWithOnConflict(name, nullColumnHack, values, 0);
    }
    public long insertWithOnConflict(String name, String nullColumnHack, ContentValues values, int conflict) {
        checkWritable();
        Table table = table(name);
        ContentValues row = new ContentValues(table.defaults);
        row.putAll(values);
        if (table.keys.size() == 1 && "_id".equals(table.keys.get(0)) && row.get("_id") == null) {
            row.put("_id", table.nextId);
        }
        if (!table.keys.isEmpty()) {
            for (int i = 0; i < table.rows.size(); ++i) {
                ContentValues existing = table.rows.get(i);
                boolean equal = true;
                for (String key : table.keys) equal &= Objects.equals(row.getAsString(key), existing.getAsString(key));
                if (!equal) continue;
                if (conflict != CONFLICT_REPLACE) return -1;
                table.rows.remove(i);
                break;
            }
        }
        Long id = row.getAsLong("_id");
        long rowId = id != null ? id : table.nextId;
        table.nextId = Math.max(table.nextId, rowId + 1);
        table.rows.add(row);
        return rowId;
    }

    public int delete(String name, String selection, String[] args) {
        checkWritable();
        Table table = table(name);
        int removed = 0;
        for (int i = table.rows.size() - 1; i >= 0; --i) {
            if (matches(table.rows.get(i), selection, args)) {
                table.rows.remove(i);
                removed++;
            }
        }
        return removed;
    }
    public int update(String name, ContentValues values, String selection, String[] args) {
        checkWritable();
        int updated = 0;
        for (ContentValues row : table(name).rows) {
            if (matches(row, selection, args)) {
                row.putAll(values);
                updated++;
            }
        }
        return updated;
    }
    public Cursor query(String name, String[] columns, String selection, String[] args,
                        String group, String having, String order) {
        return query(false, name, columns, selection, args, group, having, order, null, null);
    }
    public Cursor query(boolean distinct, String name, String[] columns, String selection, String[] args,
                        String group, String having, String order, String limit) {
        return query(distinct, name, columns, selection, args, group, having, order, limit, null);
    }
    public Cursor query(boolean distinct, String name, String[] columns, String selection, String[] args,
                        String group, String having, String order, String limit, CancellationSignal signal) {
        if (signal != null) signal.throwIfCanceled();
        if (group != null || having != null || order != null) throw new SQLiteException("Unsupported query clauses");
        Table table = "sqlite_master".equals(name) ? schemaTable() : table(name);
        String[] projection = columns != null ? columns : table.columns.toArray(new String[0]);
        MatrixCursor cursor = new MatrixCursor(projection);
        java.util.Set<java.util.List<Object>> seen = distinct ? new java.util.HashSet<>() : null;
        int maximum = limit == null ? Integer.MAX_VALUE : Integer.parseInt(limit);
        for (ContentValues row : table.rows) {
            if (cursor.getCount() >= maximum) break;
            if (!matches(row, selection, args)) continue;
            Object[] values = new Object[projection.length];
            for (int i = 0; i < projection.length; ++i) values[i] = row.get(projection[i]);
            if (seen == null || seen.add(Arrays.asList(values))) cursor.addRow(values);
        }
        return cursor;
    }

    private Table schemaTable() {
        checkOpen();
        Table schema = new Table();
        schema.columns.addAll(Arrays.asList("type", "name", "tbl_name"));
        for (String tableName : data.tables.keySet()) {
            ContentValues row = new ContentValues();
            row.put("type", "table");
            row.put("name", tableName);
            row.put("tbl_name", tableName);
            schema.rows.add(row);
        }
        return schema;
    }

    public SQLiteStatement compileStatement(String sql) {
        checkOpen();
        return new SQLiteStatement(this, sql);
    }
    long queryLong(String sql, String[] args) {
        String query = sql.trim();
        String upper = query.toUpperCase(Locale.ROOT);
        int close = query.indexOf(')');
        int from = upper.indexOf(" FROM ", close);
        if (!upper.startsWith("SELECT MAX(") || close <= 11 || from < 0) {
            throw new SQLiteException("Unsupported scalar query " + sql);
        }
        String column = query.substring(11, close).trim();
        int where = upper.indexOf(" WHERE ", from);
        String name = query.substring(from + 6, where < 0 ? query.length() : where).trim();
        String selection = where < 0 ? null : query.substring(where + 7);
        Long largest = null;
        for (ContentValues row : table(name).rows) {
            if (!matches(row, selection, args)) continue;
            Long value = row.getAsLong(column);
            if (value != null && (largest == null || value > largest)) largest = value;
        }
        return largest == null ? 0 : largest;
    }

    private static boolean matches(ContentValues row, String selection, String[] args) {
        if (selection == null || selection.isEmpty()) return true;
        int argument = 0;
        for (String term : selection.split("\\s+[Aa][Nn][Dd]\\s+")) {
            int in = term.toUpperCase(Locale.ROOT).indexOf(" IN (");
            if (in > 0 && term.endsWith(")")) {
                String key = term.substring(0, in).trim();
                String stored = row.getAsString(key);
                String candidates = term.substring(in + 5, term.length() - 1).trim();
                boolean found = false;
                if (candidates.isEmpty()) return false;
                for (String candidate : candidates.split(",")) {
                    String value = candidate.trim();
                    if ("?".equals(value)) {
                        if (args == null || argument >= args.length) throw new SQLiteException("Missing selection argument");
                        value = args[argument++];
                    } else if (value.length() >= 2 && value.charAt(0) == '\'' && value.endsWith("'")) {
                        value = value.substring(1, value.length() - 1);
                    }
                    found |= stored != null && stored.equals(value);
                }
                if (!found) return false;
                continue;
            }
            int equal = term.indexOf('=');
            if (equal < 1) throw new SQLiteException("Unsupported selection " + selection);
            String key = term.substring(0, equal).trim();
            char comparison = key.charAt(key.length() - 1);
            if (comparison == '!' || comparison == '<' || comparison == '>') key = key.substring(0, key.length() - 1).trim();
            String value = term.substring(equal + (term.startsWith("==", equal) ? 2 : 1)).trim();
            if ("?".equals(value)) {
                if (args == null || argument >= args.length) throw new SQLiteException("Missing selection argument");
                value = args[argument++];
            } else if (value.length() >= 2 && value.charAt(0) == '\'' && value.endsWith("'")) {
                value = value.substring(1, value.length() - 1);
            }
            String stored = row.getAsString(key);
            if (comparison == '<' || comparison == '>') {
                if (stored == null || value == null) return false;
                long left = Long.parseLong(stored), right = Long.parseLong(value);
                if (comparison == '<' ? left > right : left < right) return false;
            } else if ((comparison != '!') != Objects.equals(value, stored)) {
                return false;
            }
        }
        return true;
    }
}
