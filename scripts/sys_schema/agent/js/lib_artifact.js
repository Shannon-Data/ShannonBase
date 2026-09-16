//@include lib_memory_registry.js
//@include lib_usage.js

/* ===========================================================================
 * The artifact store: where a tool result goes when it is too big to say.
 *
 * Every tool result used to be squeezed through compress(text, 1200) and the
 * remainder discarded.  For a schema dump, an EXPLAIN tree or any query
 * returning more than a handful of rows, the model was handed a fragment that
 * stopped mid-row, with no indication of what was missing and no way to ask.
 * The usual symptom was the model re-running the same query with a smaller
 * LIMIT and guessing at the rest.
 *
 * So the full text is written here and the model is given a preview plus a
 * handle.  read_artifact pages through the rest.  Three things follow from
 * that which are worth stating, because they are what makes this a store
 * rather than a variable:
 *
 *   1. Isolation.  principal_prefix is on every read predicate.  A handle is
 *      not a capability: quoting someone else's artifact_id returns nothing.
 *   2. Deduplication.  The unique key is (principal_prefix, content_hash), so
 *      running the same query on ten turns stores one copy.  This is the
 *      first consumer of the content_hash idea that agent_memory has carried
 *      unread since it was added.
 *   3. Retention.  Artifacts are the only unbounded payload the agent writes,
 *      so they expire by default and the purge runs in the same maintenance
 *      slot as the rest of memory.
 * ======================================================================== */

var ARTIFACT_DEFAULTS = {
  /* Below this, a result is just said rather than stored: a handle the model
   * has to spend a turn dereferencing is worse than the text itself. */
  spill_threshold_chars: 1200,
  /* What the model sees inline once a result has spilled.  Deliberately
   * smaller than the spill threshold -- the preview exists to let the model
   * decide whether to page, not to substitute for paging. */
  preview_chars:         800,
  /* One read_artifact call's worth. */
  page_chars:            4000,
  /* Hard ceiling on a stored artifact.  A result larger than this is stored
   * truncated and flagged, rather than refused: a truncated answer the model
   * knows is truncated beats no answer. */
  max_bytes:             1048576,
  ttl_days:              7,
  enabled:               true
};

function artifact_options(mo) {
  var a = (mo && mo.artifact) || {};
  var d = ARTIFACT_DEFAULTS;
  return {
    enabled:               a.enabled !== false,
    spill_threshold_chars: Math.max(200,  mem_num(a.spill_threshold_chars, d.spill_threshold_chars)),
    preview_chars:         Math.max(100,  mem_num(a.preview_chars,         d.preview_chars)),
    page_chars:            Math.max(200,  mem_num(a.page_chars,            d.page_chars)),
    max_bytes:             Math.max(4096, mem_num(a.max_bytes,             d.max_bytes)),
    ttl_days:              Math.max(0,    mem_num(a.ttl_days,              d.ttl_days))
  };
}

/* The payload travels through a session variable rather than being inlined
 * twice.  artifact_id and content_hash are both derived from the content, so
 * a literal would otherwise appear three times in two statements -- three
 * copies of a megabyte through the SQL parser to store one. */
var ARTIFACT_CONTENT_VAR = '@_shannon_artifact_content';

function artifact_put(kind, mime, content, meta, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var ao = artifact_options(mo);
  if (!ao.enabled) return { ok: false, error: 'artifacts_disabled' };

  var prefix = mem_principal_prefix();
  if (!prefix) {
    mem_log_audit('L2', 'degraded', 'mysql.agent_artifact', 'missing_isolation_key', 0, 0, mo.vector_index);
    return { ok: false, error: 'missing_isolation_key' };
  }

  var text      = String(content == null ? '' : content);
  var full_len  = text.length;
  var truncated = 0;
  if (full_len > ao.max_bytes) {
    text = text.substring(0, ao.max_bytes);
    /* substring() counts UTF-16 code units, so the cut can land between the
     * two halves of a surrogate pair -- an emoji or a supplementary-plane CJK
     * character -- and leave a lone surrogate behind.  That is not encodable
     * as UTF-8, so the server rejects the whole INSERT (or silently stores a
     * replacement character): a megabyte of result thrown away to save the
     * last half character. */
    if (/[\uD800-\uDBFF]$/.test(text)) text = text.substring(0, text.length - 1);
    truncated = 1;
  }
  text = mem_redact(text, mo);
  if (!text) return { ok: false, error: 'empty_content' };

  meta = meta || {};
  var expires = (ao.ttl_days > 0) ? "DATE_ADD(NOW(), INTERVAL " + ao.ttl_days + " DAY)" : 'NULL';

  try {
    query_checked("SET " + ARTIFACT_CONTENT_VAR + " = '" + esc(text) + "'");
    /* A plain VALUES row rather than INSERT ... SELECT: the columns are all
     * expressions over the session variable and constants, so there is
     * nothing to select from, and ON DUPLICATE KEY UPDATE's VALUES() reads
     * cleanly here.  (The row-alias spelling that replaces VALUES() in 8.0.20+
     * is not available to INSERT ... SELECT at all, so this form is also the
     * one with a future.) */
    query_checked(
      "INSERT INTO mysql.agent_artifact" +
      " (artifact_id, principal_prefix, conversation_id, turn_id, kind, mime, content," +
      "  size_bytes, row_count, truncated, content_hash, source_sql, meta, expires_at)" +
      " VALUES (CONCAT('art_', SUBSTRING(SHA2(CONCAT('" + esc(prefix) + "'," +
      ARTIFACT_CONTENT_VAR + "),256),1,32))," +
      " '" + esc(prefix) + "','" + esc(A.conversation_id || '') + "'," +
      " '" + esc(current_turn_id()) + "'," +
      " '" + esc(String(kind || 'text').substring(0, 16)) + "'," +
      " '" + esc(String(mime || 'text/plain').substring(0, 64)) + "'," +
      " " + ARTIFACT_CONTENT_VAR + "," +
      " CHAR_LENGTH(" + ARTIFACT_CONTENT_VAR + ")," + Number(meta.row_count || 0) + "," + truncated + "," +
      " SHA2(" + ARTIFACT_CONTENT_VAR + ",256)," +
      (meta.source_sql ? "'" + esc(String(meta.source_sql).substring(0, 4000)) + "'" : 'NULL') + "," +
      " CAST('" + esc(JSON.stringify(meta)) + "' AS JSON)," + expires + ")" +
      /* A repeat of the same result is the same artifact.  Refresh its expiry
       * so an artifact the conversation keeps producing does not expire out
       * from under a handle the model is still holding.
       *
       * Provenance moves as one unit or not at all.  Refreshing turn_id alone
       * left the row describing two different producers -- source_sql from
       * the first turn that stored this content, turn_id from the last -- and
       * the same text can legitimately come from a different query.  The
       * content-derived columns (size_bytes, row_count, content_hash,
       * truncated) are deliberately not touched: the content is identical by
       * definition of the key, and `truncated` is a property of that stored
       * text, so a later producer that happened not to hit the cap must not
       * relabel it as complete.  created_at stays first-seen. */
      " ON DUPLICATE KEY UPDATE expires_at=VALUES(expires_at), turn_id=VALUES(turn_id)," +
      " conversation_id=VALUES(conversation_id), source_sql=VALUES(source_sql)," +
      " meta=VALUES(meta)");

    var rows = query(
      "SELECT artifact_id, size_bytes, truncated FROM mysql.agent_artifact" +
      " WHERE principal_prefix='" + esc(prefix) + "'" +
      "   AND content_hash=SHA2(" + ARTIFACT_CONTENT_VAR + ",256)");
    query_checked("SET " + ARTIFACT_CONTENT_VAR + " = NULL");

    if (!Array.isArray(rows) || !rows.length)
      return { ok: false, error: 'artifact_not_readable_after_write' };

    var id = String(rows[0].artifact_id);
    mem_log_audit('L2', 'write', 'mysql.agent_artifact',
                  id + ' kind=' + kind + ' bytes=' + rows[0].size_bytes, 1, 0, mo.vector_index);
    usage_add({ artifact_bytes: Number(rows[0].size_bytes || 0) });
    return { ok: true, artifact_id: id,
             size_bytes: Number(rows[0].size_bytes || 0),
             original_bytes: full_len,
             truncated: !!Number(rows[0].truncated) };
  } catch (e) {
    try { query_checked("SET " + ARTIFACT_CONTENT_VAR + " = NULL"); } catch (e2) {}
    mem_log_audit('L2', 'degraded', 'mysql.agent_artifact', String(e).substring(0, 200), 0, 0, mo.vector_index);
    return { ok: false, error: String(e) };
  }
}

/* Read one page.  SUBSTRING rather than reading the whole column and slicing
 * in JavaScript: the point of the store is that the payload does not have to
 * fit anywhere except the table. */
function artifact_read(artifact_id, offset, limit, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var ao = artifact_options(mo);
  var prefix = mem_principal_prefix();
  if (!prefix) return { ok: false, error: 'missing_isolation_key' };

  var id  = String(artifact_id || '').trim();
  if (!/^[A-Za-z0-9_]{1,64}$/.test(id)) return { ok: false, error: 'invalid_artifact_id' };
  var off = Math.max(0, Math.floor(mem_num(offset, 0)));
  var len = Math.max(1, Math.min(Math.floor(mem_num(limit, ao.page_chars)), ao.page_chars));

  /* SUBSTRING is 1-based. */
  var rows = query(
    "SELECT kind, mime, size_bytes, row_count, truncated, source_sql," +
    " SUBSTRING(content, " + (off + 1) + ", " + len + ") AS page" +
    " FROM mysql.agent_artifact" +
    " WHERE artifact_id='" + esc(id) + "' AND principal_prefix='" + esc(prefix) + "'" +
    "   AND (expires_at IS NULL OR expires_at > NOW())");
  if (!Array.isArray(rows)) return { ok: false, error: 'artifact_read_failed' };
  /* Not found and not yours are the same answer on purpose: a handle must not
   * be usable to probe whether another principal holds one. */
  if (!rows.length) return { ok: false, error: 'artifact_not_found' };

  var size = Number(rows[0].size_bytes || 0);
  var page = String(rows[0].page || '');
  try {
    query_checked("UPDATE mysql.agent_artifact SET read_count=read_count+1" +
                  " WHERE artifact_id='" + esc(id) + "' AND principal_prefix='" + esc(prefix) + "'");
  } catch (e) { /* usage stats are best effort */ }

  return { ok: true, artifact_id: id, kind: String(rows[0].kind || ''),
           mime: String(rows[0].mime || ''), size_bytes: size,
           row_count: Number(rows[0].row_count || 0),
           truncated: !!Number(rows[0].truncated),
           source_sql: rows[0].source_sql ? String(rows[0].source_sql) : '',
           offset: off, length: page.length,
           next_offset: (off + page.length < size) ? (off + page.length) : null,
           eof: (off + page.length) >= size,
           text: page };
}

function artifact_purge_expired(opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.retention || mo.retention.enabled === false) return 0;
  var prefix = mem_principal_prefix();
  if (!prefix) return 0;
  try {
    var r = query_checked(
      "DELETE FROM mysql.agent_artifact WHERE principal_prefix='" + esc(prefix) + "'" +
      " AND expires_at IS NOT NULL AND expires_at < NOW()" +
      " LIMIT " + Number(mo.retention.purge_batch || 500));
    var n = (r && r.affected_rows) ? Number(r.affected_rows) : 0;
    if (n) mem_log_audit('L2', 'purge', 'mysql.agent_artifact', 'expired', n, 0, mo.vector_index);
    return n;
  } catch (e) { return 0; }
}

/* The seam every tool result passes through.
 *
 * Returns the text to show the model: the result itself when it is small
 * enough to say, or a preview plus a handle when it is not.  Callers get one
 * behaviour change and no new control flow -- which is why this returns a
 * string rather than a decision for the caller to act on.
 *
 * A failed spill falls back to the old truncation rather than to an error:
 * losing the tail of a result is a degradation, losing the result is a bug. */
function artifact_spill(text, kind, meta, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var ao = artifact_options(mo);
  var s  = String(text == null ? '' : text);
  if (!ao.enabled || s.length <= ao.spill_threshold_chars) return s;

  var put = artifact_put(kind || 'result_set', 'text/plain', s, meta, mo);
  if (!put.ok) return compress(s, ao.spill_threshold_chars);

  return compress(s, ao.preview_chars) + '\n' +
    t('【完整结果已存档】artifact_id=' + put.artifact_id +
      '，共 ' + put.original_bytes + ' 字符' +
      (put.truncated ? '（已截断到 ' + put.size_bytes + '）' : '') +
      '。需要余下内容时调用 read_artifact，不要重跑查询。',
      '[Full result stored] artifact_id=' + put.artifact_id +
      ', ' + put.original_bytes + ' chars' +
      (put.truncated ? ' (stored truncated at ' + put.size_bytes + ')' : '') +
      '. Call read_artifact for the rest -- do not re-run the query.');
}



/* rows_to_table()'s own default row cap.  A result longer than this was
 * already being silently cut off before the character truncation ever ran,
 * which is why "spill when the text is long" is not on its own the right
 * test: 10000 narrow rows render to well under the character threshold and
 * still lose 9850 rows. */
var ARTIFACT_ROWS_INLINE = 150;
/* What goes into the artifact.  Bounded because rendering is done in
 * JavaScript, in the server, on the user's turn. */
var ARTIFACT_ROWS_MAX = 5000;

/* Render a result set for the model, spilling to the artifact store when it
 * does not fit.  The single place query results become text. */
function artifact_render_result(rows, sql, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var ao = artifact_options(mo);
  var n  = Array.isArray(rows) ? rows.length : 0;
  var text = rows_to_table(rows);

  if (!ao.enabled) return compress(text, ao.spill_threshold_chars);
  if (n <= ARTIFACT_ROWS_INLINE && text.length <= ao.spill_threshold_chars) return text;

  var full = (n > ARTIFACT_ROWS_INLINE) ? rows_to_table(rows, ARTIFACT_ROWS_MAX) : text;
  return artifact_spill(full, 'result_set', { row_count: n, source_sql: sql }, mo);
}

var ARTIFACT = {
  put: artifact_put, read: artifact_read, spill: artifact_spill,
  render_result: artifact_render_result,
  purge_expired: artifact_purge_expired, options: artifact_options
};
