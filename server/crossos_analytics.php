<?php
/**
 * crossos_analytics.php  –  CrossOS debug telemetry & analytics server.
 *
 * Upload this single file to: my-site-name.com/crossos/index.php
 * It will be accessible as:   my-site-name.com/crossos
 *
 * Features
 * ────────
 *  • Accepts batched log and analytics-event payloads from CrossOS debug
 *    builds via authenticated HTTP POST.
 *  • Stores data in a SQLite (default) or MySQL database.
 *  • Auto-purges entries older than 30 days; warns when storage exceeds 1 GB.
 *  • Exposes a Model Context Protocol (MCP) endpoint (JSON-RPC 2.0) so an
 *    LLM assistant can query the data to help debug real-device issues.
 *
 * ── Quick start ────────────────────────────────────────────────────────────
 *  1. Edit the CONFIG section below (set TELEMETRY_SECRET to the same value
 *     as CROSSOS_TELEMETRY_SECRET_DEFAULT in your C build).
 *  2. Upload to your shared host.
 *  3. The PHP file self-initialises the database on first request.
 *
 * ── Auth ───────────────────────────────────────────────────────────────────
 *  The C client sends: auth = sha256( secret + ":" + floor(unix_time / 300) )
 *  The server verifies current window ± 1 (5-minute tolerance for clock skew).
 *  For MCP access use Bearer token: Authorization: Bearer <MCP_SECRET>
 *
 * ── Endpoints ──────────────────────────────────────────────────────────────
 *  POST /crossos          Ingest a batch of log/event entries (see C client).
 *  POST /crossos?mcp=1    MCP JSON-RPC 2.0 – tools for LLM-assisted debugging.
 *  GET  /crossos?ping=1   Health-check; returns {"ok":true,"storage_mb":…}.
 */

declare(strict_types=1);

/* ══════════════════════════════════════════════════════════════════════════
 *  CONFIG  (edit these before deploying)
 * ══════════════════════════════════════════════════════════════════════════ */

/** Must match CROSSOS_TELEMETRY_SECRET_DEFAULT in your C CMake configuration. */
const TELEMETRY_SECRET = 'Xk9m2pQr7vY3E4wA';

/** Secret for MCP access (set a long random string; keep private). */
const MCP_SECRET = 'kapsodpioiioj309mansd093n2u4r8v7w6x5y';

/** Database driver: 'sqlite' or 'mysql'. */
const DB_DRIVER = 'sqlite';

/** Maximum iterations when trimming to stay under STORAGE_CAP_BYTES. */
const MAX_PURGE_ITERATIONS = 20;

/** SQLite: path to the database file (must be writable; outside web-root is better). */
const SQLITE_PATH = __DIR__ . '/crossos_analytics.sqlite3';

/** MySQL: connection settings (only used when DB_DRIVER = 'mysql'). */
const MYSQL_HOST = 'localhost';
const MYSQL_PORT = 3306;
const MYSQL_DB   = 'crossos_analytics';
const MYSQL_USER = 'db_user';
const MYSQL_PASS = 'db_pass';

/** Retention window in days. */
const RETAIN_DAYS = 30;

/** Storage cap in bytes (1 GB). Older rows are pruned when exceeded. */
const STORAGE_CAP_BYTES = 1_073_741_824;

/* ══════════════════════════════════════════════════════════════════════════
 *  Bootstrap
 * ══════════════════════════════════════════════════════════════════════════ */

header('Content-Type: application/json; charset=utf-8');
header('X-Content-Type-Options: nosniff');

/* Guard against deploying with the default MCP secret unchanged. */
if (MCP_SECRET === 'change-me-mcp-secret-2026') {
    /* Still allow ingest and ping, but block MCP until the secret is changed. */
    define('MCP_SECRET_UNSAFE', true);
} else {
    define('MCP_SECRET_UNSAFE', false);
}

try {
    $db = db_connect();
    db_init($db);
    db_purge($db);
    route($db);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode(['error' => 'internal_error', 'detail' => $e->getMessage()]);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Router
 * ══════════════════════════════════════════════════════════════════════════ */

function route(PDO $db): void
{
    /* Health check */
    if (isset($_GET['ping'])) {
        echo json_encode(['ok' => true, 'storage_mb' => round(db_size_bytes($db) / 1048576, 2)]);
        return;
    }

    /* MCP endpoint */
    if (isset($_GET['mcp'])) {
        auth_mcp();
        $body = json_decode(file_get_contents('php://input') ?: '{}', true) ?? [];
        echo json_encode(mcp_dispatch($db, $body));
        return;
    }

    /* Ingest endpoint */
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
        http_response_code(405);
        echo json_encode(['error' => 'method_not_allowed']);
        return;
    }

    $body = json_decode(file_get_contents('php://input') ?: '{}', true);
    if (!is_array($body)) {
        http_response_code(400);
        echo json_encode(['error' => 'invalid_json']);
        return;
    }

    auth_ingest($body);
    ingest($db, $body);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Authentication
 * ══════════════════════════════════════════════════════════════════════════ */

/** Verify time-windowed token sent by the C client. */
function auth_ingest(array $body): void
{
    $token = (string)($body['auth'] ?? '');
    if (!verify_ingest_token($token)) {
        http_response_code(401);
        echo json_encode(['error' => 'unauthorized']);
        exit;
    }
}

/** Check ± 1 time window (5-minute buckets, same algorithm as C client). */
function verify_ingest_token(string $token): bool
{
    if (strlen($token) !== 64) return false;
    $now = (int)(time() / 300);
    foreach ([$now - 1, $now, $now + 1] as $w) {
        $expected = hash('sha256', TELEMETRY_SECRET . ':' . $w);
        if (hash_equals($expected, strtolower($token))) return true;
    }
    return false;
}

/** Verify Bearer token for MCP access. */
function auth_mcp(): void
{
    if (MCP_SECRET_UNSAFE) {
        http_response_code(503);
        echo json_encode([
            'error'  => 'mcp_disabled',
            'detail' => 'MCP_SECRET has not been changed from the default. '
                      . 'Edit the CONFIG section of crossos_analytics.php before using MCP.',
        ]);
        exit;
    }

    $auth = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
    if (!str_starts_with($auth, 'Bearer ')) {
        http_response_code(401);
        echo json_encode(['error' => 'mcp_unauthorized']);
        exit;
    }
    $provided = substr($auth, 7);
    if (!hash_equals(MCP_SECRET, $provided)) {
        http_response_code(403);
        echo json_encode(['error' => 'mcp_forbidden']);
        exit;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Ingest
 * ══════════════════════════════════════════════════════════════════════════ */

function ingest(PDO $db, array $body): void
{
    $device_id   = substr((string)($body['device_id']   ?? 'unknown'), 0, 128);
    $platform    = substr((string)($body['platform']    ?? 'unknown'), 0, 32);
    $app_version = substr((string)($body['app_version'] ?? ''),        0, 64);
    $batch       = $body['batch'] ?? [];

    if (!is_array($batch)) {
        http_response_code(400);
        echo json_encode(['error' => 'batch_must_be_array']);
        return;
    }

    $inserted = 0;
    $db->beginTransaction();
    try {
        foreach ($batch as $entry) {
            if (!is_array($entry)) continue;
            $type = (string)($entry['type'] ?? '');
            $ts   = (int)($entry['ts']   ?? time());

            if ($type === 'log') {
                $stmt = $db->prepare(
                    'INSERT INTO crossos_logs
                       (created_at, device_id, platform, app_version, level, tag, message)
                     VALUES (datetime(:ts,"unixepoch"), :dev, :plat, :ver, :lvl, :tag, :msg)'
                );
                $stmt->execute([
                    ':ts'  => $ts,
                    ':dev' => $device_id,
                    ':plat'=> $platform,
                    ':ver' => $app_version,
                    ':lvl' => substr((string)($entry['level']   ?? 'info'),    0, 8),
                    ':tag' => substr((string)($entry['tag']     ?? ''),         0, 64),
                    ':msg' => substr((string)($entry['message'] ?? ''),         0, 2048),
                ]);
                $inserted++;
            } elseif ($type === 'event') {
                $props = $entry['props'] ?? null;
                if (is_array($props)) $props = json_encode($props);
                $stmt = $db->prepare(
                    'INSERT INTO crossos_events
                       (created_at, device_id, platform, app_version, event_name, properties)
                     VALUES (datetime(:ts,"unixepoch"), :dev, :plat, :ver, :name, :props)'
                );
                $stmt->execute([
                    ':ts'   => $ts,
                    ':dev'  => $device_id,
                    ':plat' => $platform,
                    ':ver'  => $app_version,
                    ':name' => substr((string)($entry['name'] ?? ''), 0, 128),
                    ':props'=> $props !== null ? substr((string)$props, 0, 4096) : null,
                ]);
                $inserted++;
            }
        }
        $db->commit();
    } catch (Throwable $e) {
        $db->rollBack();
        throw $e;
    }

    echo json_encode(['ok' => true, 'inserted' => $inserted]);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  MCP – Model Context Protocol (JSON-RPC 2.0)
 * ══════════════════════════════════════════════════════════════════════════
 *
 *  Tools exposed to the LLM:
 *    query_logs       – filter log entries (device, level, tag, time range)
 *    query_events     – filter analytics events
 *    get_summary      – device / platform / level breakdown counts
 *    get_recent_errors– shortcut: last N error-level log entries
 *    purge_old_data   – manually trigger retention cleanup
 */

function mcp_dispatch(PDO $db, array $rpc): array
{
    $id     = $rpc['id']     ?? null;
    $method = $rpc['method'] ?? '';
    $params = $rpc['params'] ?? [];

    /* Validate JSON-RPC envelope */
    if (($rpc['jsonrpc'] ?? '') !== '2.0') {
        return mcp_error($id, -32600, 'Invalid Request: jsonrpc must be "2.0"');
    }

    switch ($method) {
        case 'initialize':
            return mcp_ok($id, [
                'protocolVersion' => '2024-11-05',
                'capabilities'    => ['tools' => new stdClass()],
                'serverInfo'      => ['name' => 'crossos-analytics', 'version' => '1.0.0'],
            ]);

        case 'notifications/initialized':
            /* No response needed for notifications; return empty to satisfy transport. */
            return ['jsonrpc' => '2.0'];

        case 'tools/list':
            return mcp_ok($id, ['tools' => mcp_tool_list()]);

        case 'tools/call':
            $name      = (string)($params['name']      ?? '');
            $arguments = (array) ($params['arguments'] ?? []);
            return mcp_call_tool($db, $id, $name, $arguments);

        default:
            return mcp_error($id, -32601, 'Method not found: ' . $method);
    }
}

function mcp_tool_list(): array
{
    return [
        [
            'name'        => 'query_logs',
            'description' => 'Query CrossOS device log entries with optional filters. '
                           . 'Returns up to `limit` rows (default 100, max 500).',
            'inputSchema' => [
                'type'       => 'object',
                'properties' => [
                    'device_id'   => ['type' => 'string',  'description' => 'Filter by device identifier.'],
                    'platform'    => ['type' => 'string',  'description' => 'Filter by platform: windows|linux|android.'],
                    'level'       => ['type' => 'string',  'enum' => ['debug','info','warn','error'],
                                      'description' => 'Minimum log level.'],
                    'tag'         => ['type' => 'string',  'description' => 'Filter by log tag (exact match).'],
                    'message_like'=> ['type' => 'string',  'description' => 'SQL LIKE pattern for the message field.'],
                    'since_hours' => ['type' => 'integer', 'description' => 'Only return entries from the last N hours. Default 24.'],
                    'limit'       => ['type' => 'integer', 'description' => 'Max rows to return (1–500). Default 100.'],
                    'offset'      => ['type' => 'integer', 'description' => 'Pagination offset.'],
                ],
            ],
        ],
        [
            'name'        => 'query_events',
            'description' => 'Query CrossOS analytics events with optional filters.',
            'inputSchema' => [
                'type'       => 'object',
                'properties' => [
                    'device_id'  => ['type' => 'string',  'description' => 'Filter by device identifier.'],
                    'platform'   => ['type' => 'string',  'description' => 'Filter by platform.'],
                    'event_name' => ['type' => 'string',  'description' => 'Filter by event name (exact match).'],
                    'since_hours'=> ['type' => 'integer', 'description' => 'Only return events from the last N hours. Default 24.'],
                    'limit'      => ['type' => 'integer', 'description' => 'Max rows to return (1–500). Default 100.'],
                    'offset'     => ['type' => 'integer', 'description' => 'Pagination offset.'],
                ],
            ],
        ],
        [
            'name'        => 'get_summary',
            'description' => 'Return aggregate counts grouped by device, platform, and log level. '
                           . 'Useful as a first look at which devices are generating issues.',
            'inputSchema' => [
                'type'       => 'object',
                'properties' => [
                    'since_hours' => ['type' => 'integer', 'description' => 'Look-back window in hours. Default 24.'],
                ],
            ],
        ],
        [
            'name'        => 'get_recent_errors',
            'description' => 'Shortcut: return the most recent error-level log entries across all devices.',
            'inputSchema' => [
                'type'       => 'object',
                'properties' => [
                    'limit'       => ['type' => 'integer', 'description' => 'Max rows (1–200). Default 50.'],
                    'since_hours' => ['type' => 'integer', 'description' => 'Look-back window in hours. Default 48.'],
                ],
            ],
        ],
        [
            'name'        => 'purge_old_data',
            'description' => 'Manually delete log and event entries older than the retention window ('
                           . RETAIN_DAYS . ' days) and any excess beyond the storage cap.',
            'inputSchema' => ['type' => 'object', 'properties' => []],
        ],
    ];
}

function mcp_call_tool(PDO $db, mixed $id, string $name, array $args): array
{
    switch ($name) {
        case 'query_logs':
            return mcp_ok($id, ['content' => [['type' => 'text',
                'text' => json_encode(tool_query_logs($db, $args), JSON_PRETTY_PRINT)]]]);

        case 'query_events':
            return mcp_ok($id, ['content' => [['type' => 'text',
                'text' => json_encode(tool_query_events($db, $args), JSON_PRETTY_PRINT)]]]);

        case 'get_summary':
            return mcp_ok($id, ['content' => [['type' => 'text',
                'text' => json_encode(tool_get_summary($db, $args), JSON_PRETTY_PRINT)]]]);

        case 'get_recent_errors':
            $args['level'] = 'error';
            if (!isset($args['since_hours'])) $args['since_hours'] = 48;
            if (!isset($args['limit']))       $args['limit']       = 50;
            return mcp_ok($id, ['content' => [['type' => 'text',
                'text' => json_encode(tool_query_logs($db, $args), JSON_PRETTY_PRINT)]]]);

        case 'purge_old_data':
            $deleted = db_purge($db);
            return mcp_ok($id, ['content' => [['type' => 'text',
                'text' => json_encode(['purged_rows' => $deleted,
                    'storage_mb' => round(db_size_bytes($db) / 1048576, 2)])]]]);

        default:
            return mcp_error($id, -32602, 'Unknown tool: ' . $name);
    }
}

/* ── Tool implementations ─────────────────────────────────────────────── */

function tool_query_logs(PDO $db, array $args): array
{
    $limit  = min(500, max(1, (int)($args['limit']       ?? 100)));
    $offset = max(0,         (int)($args['offset']       ?? 0));
    $hours  = max(1,         (int)($args['since_hours']  ?? 24));

    $where  = ["created_at >= datetime('now', '-{$hours} hours')"];
    $bind   = [];

    if (!empty($args['device_id'])) {
        $where[] = 'device_id = :device_id';
        $bind[':device_id'] = (string)$args['device_id'];
    }
    if (!empty($args['platform'])) {
        $where[] = 'platform = :platform';
        $bind[':platform'] = (string)$args['platform'];
    }
    if (!empty($args['level'])) {
        /* Return the requested level and all levels above it. */
        $levels = level_and_above((string)$args['level']);
        $placeholders = implode(',', array_map(static fn($i) => ":lvl$i", array_keys($levels)));
        $where[] = "level IN ($placeholders)";
        foreach ($levels as $i => $l) $bind[":lvl$i"] = $l;
    }
    if (!empty($args['tag'])) {
        $where[] = 'tag = :tag';
        $bind[':tag'] = (string)$args['tag'];
    }
    if (!empty($args['message_like'])) {
        $where[] = 'message LIKE :msg';
        $bind[':msg'] = (string)$args['message_like'];
    }

    $sql = 'SELECT id, created_at, device_id, platform, app_version, level, tag, message
            FROM crossos_logs WHERE ' . implode(' AND ', $where)
         . ' ORDER BY created_at DESC LIMIT :limit OFFSET :offset';

    $stmt = $db->prepare($sql);
    foreach ($bind as $k => $v) $stmt->bindValue($k, $v);
    $stmt->bindValue(':limit',  $limit,  PDO::PARAM_INT);
    $stmt->bindValue(':offset', $offset, PDO::PARAM_INT);
    $stmt->execute();

    return $stmt->fetchAll(PDO::FETCH_ASSOC);
}

function tool_query_events(PDO $db, array $args): array
{
    $limit  = min(500, max(1, (int)($args['limit']      ?? 100)));
    $offset = max(0,         (int)($args['offset']      ?? 0));
    $hours  = max(1,         (int)($args['since_hours'] ?? 24));

    $where = ["created_at >= datetime('now', '-{$hours} hours')"];
    $bind  = [];

    if (!empty($args['device_id'])) {
        $where[] = 'device_id = :device_id';
        $bind[':device_id'] = (string)$args['device_id'];
    }
    if (!empty($args['platform'])) {
        $where[] = 'platform = :platform';
        $bind[':platform'] = (string)$args['platform'];
    }
    if (!empty($args['event_name'])) {
        $where[] = 'event_name = :event_name';
        $bind[':event_name'] = (string)$args['event_name'];
    }

    $sql = 'SELECT id, created_at, device_id, platform, app_version, event_name, properties
            FROM crossos_events WHERE ' . implode(' AND ', $where)
         . ' ORDER BY created_at DESC LIMIT :limit OFFSET :offset';

    $stmt = $db->prepare($sql);
    foreach ($bind as $k => $v) $stmt->bindValue($k, $v);
    $stmt->bindValue(':limit',  $limit,  PDO::PARAM_INT);
    $stmt->bindValue(':offset', $offset, PDO::PARAM_INT);
    $stmt->execute();

    $rows = $stmt->fetchAll(PDO::FETCH_ASSOC);
    /* Decode JSON properties inline for easier LLM reading. */
    foreach ($rows as &$row) {
        if (!empty($row['properties'])) {
            $decoded = json_decode($row['properties'], true);
            if ($decoded !== null) $row['properties'] = $decoded;
        }
    }
    return $rows;
}

function tool_get_summary(PDO $db, array $args): array
{
    $hours = max(1, (int)($args['since_hours'] ?? 24));
    $since = "datetime('now', '-{$hours} hours')";

    /* Log counts by device + platform + level */
    $log_stmt = $db->query(
        "SELECT device_id, platform, level, COUNT(*) AS count
         FROM crossos_logs WHERE created_at >= $since
         GROUP BY device_id, platform, level
         ORDER BY count DESC"
    );

    /* Event counts by device + event_name */
    $evt_stmt = $db->query(
        "SELECT device_id, platform, event_name, COUNT(*) AS count
         FROM crossos_events WHERE created_at >= $since
         GROUP BY device_id, platform, event_name
         ORDER BY count DESC"
    );

    return [
        'since_hours'    => $hours,
        'storage_mb'     => round(db_size_bytes($db) / 1048576, 2),
        'log_breakdown'  => $log_stmt->fetchAll(PDO::FETCH_ASSOC),
        'event_breakdown'=> $evt_stmt->fetchAll(PDO::FETCH_ASSOC),
    ];
}

/* ── Level ordering helper ────────────────────────────────────────────── */

function level_and_above(string $level): array
{
    $order = ['debug' => 0, 'info' => 1, 'warn' => 2, 'error' => 3];
    $min   = $order[$level] ?? 0;
    return array_values(array_keys(array_filter($order, static fn($v) => $v >= $min)));
}

/* ── JSON-RPC helpers ─────────────────────────────────────────────────── */

function mcp_ok(mixed $id, array $result): array
{
    return ['jsonrpc' => '2.0', 'id' => $id, 'result' => $result];
}

function mcp_error(mixed $id, int $code, string $message): array
{
    return ['jsonrpc' => '2.0', 'id' => $id, 'error' => ['code' => $code, 'message' => $message]];
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Database helpers
 * ══════════════════════════════════════════════════════════════════════════ */

function db_connect(): PDO
{
    if (DB_DRIVER === 'mysql') {
        $dsn = sprintf('mysql:host=%s;port=%d;dbname=%s;charset=utf8mb4',
                       MYSQL_HOST, MYSQL_PORT, MYSQL_DB);
        $pdo = new PDO($dsn, MYSQL_USER, MYSQL_PASS, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    } else {
        $pdo = new PDO('sqlite:' . SQLITE_PATH, null, null,
                       [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
        $pdo->exec('PRAGMA journal_mode=WAL');
        $pdo->exec('PRAGMA synchronous=NORMAL');
    }
    $pdo->setAttribute(PDO::ATTR_DEFAULT_FETCH_MODE, PDO::FETCH_ASSOC);
    return $pdo;
}

function db_init(PDO $db): void
{
    $db->exec('
        CREATE TABLE IF NOT EXISTS crossos_logs (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            device_id   TEXT NOT NULL,
            platform    TEXT NOT NULL,
            app_version TEXT,
            level       TEXT NOT NULL,
            tag         TEXT,
            message     TEXT NOT NULL
        )
    ');

    $db->exec('
        CREATE TABLE IF NOT EXISTS crossos_events (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            device_id   TEXT NOT NULL,
            platform    TEXT NOT NULL,
            app_version TEXT,
            event_name  TEXT NOT NULL,
            properties  TEXT
        )
    ');

    /* Indexes for common query patterns */
    $db->exec('CREATE INDEX IF NOT EXISTS idx_logs_created   ON crossos_logs   (created_at)');
    $db->exec('CREATE INDEX IF NOT EXISTS idx_logs_device    ON crossos_logs   (device_id, created_at)');
    $db->exec('CREATE INDEX IF NOT EXISTS idx_logs_level     ON crossos_logs   (level, created_at)');
    $db->exec('CREATE INDEX IF NOT EXISTS idx_events_created ON crossos_events (created_at)');
    $db->exec('CREATE INDEX IF NOT EXISTS idx_events_device  ON crossos_events (device_id, created_at)');
    $db->exec('CREATE INDEX IF NOT EXISTS idx_events_name    ON crossos_events (event_name, created_at)');
}

/**
 * Delete rows older than RETAIN_DAYS and trim to STORAGE_CAP_BYTES.
 * Returns total deleted row count.
 */
function db_purge(PDO $db): int
{
    $cutoff = "datetime('now', '-" . RETAIN_DAYS . " days')";

    $n  = (int)$db->exec("DELETE FROM crossos_logs   WHERE created_at < $cutoff");
    $n += (int)$db->exec("DELETE FROM crossos_events WHERE created_at < $cutoff");

    /* Storage cap: delete oldest rows until we are under the cap. */
    $chunk      = 1000;
    $iterations = 0;
    while (db_size_bytes($db) > STORAGE_CAP_BYTES && $iterations++ < MAX_PURGE_ITERATIONS) {
        $n += (int)$db->exec(
            "DELETE FROM crossos_logs
             WHERE id IN (SELECT id FROM crossos_logs ORDER BY created_at ASC LIMIT $chunk)"
        );
        $n += (int)$db->exec(
            "DELETE FROM crossos_events
             WHERE id IN (SELECT id FROM crossos_events ORDER BY created_at ASC LIMIT $chunk)"
        );
    }

    return $n;
}

/** Return approximate database size in bytes. */
function db_size_bytes(PDO $db): int
{
    if (DB_DRIVER === 'mysql') {
        $stmt = $db->query(
            "SELECT SUM(data_length + index_length) AS sz
             FROM information_schema.TABLES
             WHERE table_schema = DATABASE()"
        );
        return (int)($stmt->fetchColumn() ?? 0);
    }
    /* SQLite: page_count × page_size */
    $pages    = (int)$db->query('PRAGMA page_count')->fetchColumn();
    $pageSize = (int)$db->query('PRAGMA page_size')->fetchColumn();
    return $pages * $pageSize;
}
