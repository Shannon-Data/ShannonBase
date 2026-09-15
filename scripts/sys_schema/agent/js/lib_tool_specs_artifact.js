//@include lib_tool_registry.js
//@include lib_artifact.js

/* The read side of the artifact store.
 *
 * There is deliberately no write tool.  Artifacts are produced by the tools
 * that produce large results, not by the model deciding to store something:
 * a model that can write arbitrary blobs into the agent schema is a storage
 * quota problem with no upside, and everything worth keeping across turns is
 * already covered by remember_fact. */
register_tool({
  name: 'read_artifact', order: 15, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: {
    zh: '读取此前工具结果被存档的完整内容（分页）\n' +
        '当结果里出现 artifact_id=... 且需要看后续内容时使用；offset 用上一次返回的 next_offset\n' +
        '⛔ 不要为了拿完整结果去重跑原查询 —— 结果已经存下来了，重跑既慢又可能读到不同的数据',
    en: 'Page through the full content of a previously stored tool result\n' +
        'Use it when a result carried artifact_id=... and you need what follows; pass the ' +
        'next_offset returned by the previous call as offset\n' +
        '⛔ Do not re-run the original query to get the rest — it is already stored, and a ' +
        're-run is both slower and liable to see different data'
  },
  example: { artifact_id: 'art_...', offset: 0 },
  args: { type: 'object', required: ['artifact_id'],
          properties: { artifact_id: { type: 'string', minLength: 4, maxLength: 64 },
                        offset:      { type: 'integer', minimum: 0 },
                        length:      { type: 'integer', minimum: 1 } } },
  messages: {
    'artifact_id#required':  { zh: 'read_artifact 缺少 artifact_id 参数',
                               en: 'read_artifact missing artifact_id argument' },
    'artifact_id#type':      { zh: 'read_artifact 缺少 artifact_id 参数',
                               en: 'read_artifact missing artifact_id argument' },
    'artifact_id#minLength': { zh: 'read_artifact 的 artifact_id 不合法',
                               en: 'read_artifact artifact_id is not valid' },
    'offset#minimum':        { zh: 'read_artifact 的 offset 不能为负',
                               en: 'read_artifact offset cannot be negative' }
  },
  handler: impl_read_artifact
});

function impl_read_artifact(args, ctx) {
  var res = artifact_read(args.artifact_id, args.offset, args.length,
                          get_memory_options(ctx.chat_opt));
  if (!res.ok) {
    /* "Not found" covers expired, never-existed and belongs-to-someone-else,
     * and the model needs to be told what to do about it rather than left to
     * retry the same handle.  Re-running the source query is the right move
     * here and the wrong move when the artifact is still readable, which is
     * why the two messages say opposite things. */
    var known = (res.error === 'artifact_not_found');
    return { ok: false, error: res.error,
             response: known
               ? t('该存档不存在或已过期（存档默认保留 7 天）。如仍需要这份数据，请重新执行原查询。',
                   'That artifact does not exist or has expired (artifacts are kept 7 days by ' +
                   'default). Re-run the original query if you still need the data.')
               : t('读取存档失败：', 'Failed to read the artifact: ') + String(res.error || '') };
  }

  var header = t(
    '【存档 ' + res.artifact_id + '】字符 ' + res.offset + '-' +
      (res.offset + res.length) + '，共 ' + res.size_bytes +
      (res.row_count ? '（' + res.row_count + ' 行）' : '') +
      (res.truncated ? '（原始结果更大，存档时已截断）' : ''),
    '[Artifact ' + res.artifact_id + '] chars ' + res.offset + '-' +
      (res.offset + res.length) + ' of ' + res.size_bytes +
      (res.row_count ? ' (' + res.row_count + ' rows)' : '') +
      (res.truncated ? ' (the original was larger and was stored truncated)' : ''));

  var footer = res.eof
    ? t('\n【已读完】', '\n[End of artifact]')
    : t('\n【还有更多】下一次调用 read_artifact 时传 offset=' + res.next_offset,
        '\n[More follows] call read_artifact again with offset=' + res.next_offset);

  return { ok: true, response: header + '\n' + res.text + footer,
           artifact_id: res.artifact_id, next_offset: res.next_offset, eof: res.eof };
}
