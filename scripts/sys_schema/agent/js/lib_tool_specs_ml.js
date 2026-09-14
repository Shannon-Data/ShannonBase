//@include lib_tool_registry.js

/* ML / AutoML tool family.
 *
 * The write/risk/table_arg metadata that used to live in the separate
 * ML_WRITE_TOOLS constant is now carried on each spec, so tool_review_meta()
 * derives it instead of a second table drifting out of sync.
 *
 * transactional:false on every one of these is deliberate and load-bearing:
 * sys.ML_* procedures own their own transaction boundaries, and wrapping them
 * in an explicit BEGIN/COMMIT from execute_review_step() risks the same THD
 * state mismatch the original comment on ML_WRITE_TOOLS warned about. */

var ML_TOOLS_GROUP = {
  zh: '【ML/AutoML 工具】机器学习全生命周期，model_handle 为模型名称（字符串）。\n' +
      '  可通过 @chat_options.handle_model 预设模型句柄，后续调用 ml_train/ml_predict 等工具时可省略 model_handle 参数。',
  en: '[ML/AutoML Tools] Full ML lifecycle — model_handle is the model name (a string).\n' +
      '  @chat_options.handle_model can preset the handle so later ml_train/ml_predict calls may omit it.'
};

/* ---------------------------------------------------------------- ml_train */
register_tool({
  name: 'ml_train', order: 120, category: 'ml', group: ML_TOOLS_GROUP,
  write: true, ddl: false, risk: 'medium',
  tableArg: 'table_name', transactional: false,
  capabilities: ['ml', 'requires_secondary_load'],
  doc: { zh: '训练 ML 模型。model_handle 为模型名称（如 census_model），task 不指定时默认 classification\n' +
             '⚠ 调用前必须先用 check_secondary_load 确认表已 SECONDARY_LOAD\n' +
             '可选 args：model_list / exclude_model_list（模型白/黑名单），optimization_metric（优化指标），' +
             'include_column_list / exclude_column_list（特征选择）\n' +
             '也可用 args.options: {...} 传入任意 ML_TRAIN 选项（会与上述独立参数合并）；' +
             '全局默认选项可通过 @chat_options.ml_train_defaults: {...} 配置',
         en: 'Train an ML model. model_handle is the model name; task defaults to classification\n' +
             '⚠ Confirm the table is SECONDARY_LOADed via check_secondary_load first\n' +
             'Optional args: model_list / exclude_model_list, optimization_metric, ' +
             'include_column_list / exclude_column_list\n' +
             'args.options: {...} passes any ML_TRAIN option through; @chat_options.ml_train_defaults ' +
             'supplies global defaults' },
  example: { table_name: 'db.table', target_column: 'label', task: 'regression', model_handle: 'census_model' },
  args: { type: 'object', required: ['table_name', 'target_column'],
          properties: {
            table_name:    { type: 'string', minLength: 3 },
            target_column: { type: 'string', minLength: 1 },
            task:          { type: 'string',
                             enum: ['classification','regression','forecasting',
                                    'anomaly_detection','recommendation'] },
            model_handle:  { type: 'string' },
            options:       { type: 'object' }
          } },
  messages: {
    'table_name#required':    { zh: 'ml_train 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_train missing table_name argument (schema.table format)' },
    'table_name#type':        { zh: 'ml_train 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_train missing table_name argument (schema.table format)' },
    'table_name#minLength':   { zh: 'ml_train 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_train missing table_name argument (schema.table format)' },
    'target_column#required': { zh: 'ml_train 缺少 target_column 参数', en: 'ml_train missing target_column argument' },
    'target_column#type':     { zh: 'ml_train 缺少 target_column 参数', en: 'ml_train missing target_column argument' }
  },
  handler: impl_ml_train
});

function impl_ml_train(args, ctx) {
  var ml_table  = esc(String(args.table_name || ''));
  var ml_target = esc(String(args.target_column || ''));
  var ml_task   = String(args.task || 'classification').toLowerCase();
  var chat_opt  = ctx.chat_opt || get_chat_options();
  var ml_handle = String(args.model_handle ||
                  (chat_opt && chat_opt.handle_model ? String(chat_opt.handle_model) : '') || '');
  /* Merge LLM-provided options with defaults from @chat_options.ml_train_defaults. */
  var ml_defaults = (chat_opt && chat_opt.ml_train_defaults) ? chat_opt.ml_train_defaults : {};
  var ml_opts = {};
  if (args.options && typeof args.options === 'object') {
    Object.keys(args.options).forEach(function(k) { ml_opts[k] = args.options[k]; });
  }
  Object.keys(ml_defaults).forEach(function(k) {
    if (!(k in ml_opts)) ml_opts[k] = ml_defaults[k];
  });
  ml_opts.task = ml_task;
  /* Also support top-level args for common options (shorthand). */
  ['model_list','exclude_model_list','optimization_metric',
   'include_column_list','exclude_column_list','contamination',
   'supervised_submodel_options','ensemble_score','datetime_index',
   'endogenous_variables','exogenous_variables'].forEach(function(k) {
    if (args[k] !== undefined && !(k in ml_opts)) ml_opts[k] = args[k];
  });
  var ml_opt = JSON.stringify(ml_opts);

  /* ML_TRAIN requires model_handle as a SESSION VARIABLE — use a well-known
     session variable and SET it from the resolved handle. */
  var ml_handle_var = '@_shannon_ml_handle';
  var ml_set_stmt = "SET " + ml_handle_var + " = " +
      (ml_handle ? "'" + esc(ml_handle) + "'" : "NULL");
  var ml_call_sql = "CALL sys.ML_TRAIN('" + ml_table + "', '" + ml_target + "', " +
                    "CAST('" + esc(ml_opt) + "' AS JSON), " + ml_handle_var + ")";
  var ml_select_sql = "SELECT " + ml_handle_var + " AS model_handle";
  try {
    query_checked(ml_set_stmt);
    var ml_train_res = query_checked(ml_call_sql);
    var ml_handle_res = query_checked(ml_select_sql);
    var resolved_handle = (Array.isArray(ml_handle_res) && ml_handle_res.length)
      ? String(ml_handle_res[0].model_handle || ml_handle || '')
      : ml_handle;
    if (resolved_handle && chat_opt) {
      chat_opt.handle_model = resolved_handle;
      save_chat_options(chat_opt);
    }
    var ml_sql_log = ml_set_stmt + ";\n" + ml_call_sql + ";\n" + ml_select_sql;
    return { ok: true, response: t('ML_TRAIN 执行完成。\n', 'ML_TRAIN completed.\n') +
             compress(rows_to_text(ml_train_res || ml_handle_res), 2000), sql: ml_sql_log,
             model_handle: resolved_handle };
  } catch (e) {
    return { ok: false, response: t('ML_TRAIN 失败：', 'ML_TRAIN failed: ') + String(e),
             error: 'ml_train_failed' };
  }
}

/* ---------------------------------------------------------- ml_predict_row */
register_tool({
  name: 'ml_predict_row', order: 121, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '单行预测，data 是列名→值的 JSON 对象',
         en: 'Single-row prediction — data is a column→value JSON object' },
  example: { model_handle: 'my_model', data: { col1: 'val1', col2: 'val2' } },
  args: { type: 'object', required: ['model_handle', 'data'],
          properties: { model_handle: { type: 'string' },
                        data:         { type: 'object' },
                        options:      { type: 'object' } } },
  messages: {
    'model_handle#required': { zh: 'ml_predict_row 缺少 model_handle 参数', en: 'ml_predict_row missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_predict_row 缺少 model_handle 参数', en: 'ml_predict_row missing model_handle argument' },
    'data#required':         { zh: 'ml_predict_row 缺少 data 参数（JSON 对象，如 {"col1":val1,"col2":val2}）',
                               en: 'ml_predict_row missing data argument (JSON object, e.g. {"col1":val1,"col2":val2})' },
    'data#type':             { zh: 'ml_predict_row 缺少 data 参数（JSON 对象，如 {"col1":val1,"col2":val2}）',
                               en: 'ml_predict_row missing data argument (JSON object, e.g. {"col1":val1,"col2":val2})' }
  },
  handler: impl_ml_predict_row
});

function impl_ml_predict_row(args, ctx) {
  var pr_handle = esc(String(args.model_handle || ''));
  var pr_data   = JSON.stringify(args.data || {});
  var pr_opt    = args.options
    ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  var pr_sql = "SELECT sys.ML_PREDICT_ROW(CAST('" + esc(pr_data) + "' AS JSON), '" +
               pr_handle + "', " + pr_opt + ") AS prediction";
  try {
    var pr_res = query_checked(pr_sql);
    return { ok: true, response: t('预测结果：\n', 'Prediction result:\n') +
             compress(rows_to_text(pr_res), 2000), sql: pr_sql };
  } catch (e) {
    return { ok: false, response: t('ML_PREDICT_ROW 失败：', 'ML_PREDICT_ROW failed: ') + String(e),
             error: 'ml_predict_row_failed' };
  }
}

/* -------------------------------------------------------- ml_predict_table */
register_tool({
  name: 'ml_predict_table', order: 122, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_table', fallbackArg: 'table_name', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '批量预测整表，output_table 可省略',
         en: 'Batch table prediction — output_table is optional' },
  example: { table_name: 'db.test', model_handle: 'my_model', output_table: 'db.out' },
  args: { type: 'object', required: ['table_name', 'model_handle'],
          properties: { table_name:   { type: 'string', minLength: 3 },
                        model_handle: { type: 'string' },
                        output_table: { type: 'string' },
                        options:      { type: 'object' } } },
  messages: {
    'table_name#required':   { zh: 'ml_predict_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_predict_table missing table_name argument (schema.table format)' },
    'table_name#type':       { zh: 'ml_predict_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_predict_table missing table_name argument (schema.table format)' },
    'table_name#minLength':  { zh: 'ml_predict_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_predict_table missing table_name argument (schema.table format)' },
    'model_handle#required': { zh: 'ml_predict_table 缺少 model_handle 参数',
                               en: 'ml_predict_table missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_predict_table 缺少 model_handle 参数',
                               en: 'ml_predict_table missing model_handle argument' }
  },
  handler: impl_ml_predict_table
});

function impl_ml_predict_table(args, ctx) {
  var pt_table  = esc(String(args.table_name || ''));
  var pt_handle = esc(String(args.model_handle || ''));
  var pt_out    = args.output_table ? esc(String(args.output_table)) : '';
  var pt_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  if (!pt_out) {
    var pt_parts  = pt_table.split('.');
    var pt_suffix = '_predictions_' + String(Date.now() % 100000);
    pt_out = (pt_parts.length > 1 ? pt_parts[0] + '.' : '') +
             pt_parts[pt_parts.length - 1] + pt_suffix;
  }
  var pt_sql = "CALL sys.ML_PREDICT_TABLE('" + pt_table + "', '" +
               pt_handle + "', '" + pt_out + "', " + pt_opt + ")";
  try {
    var pt_res = query_checked(pt_sql);
    return { ok: true,
             response: t('ML_PREDICT_TABLE 执行完成，结果写入 ',
                         'ML_PREDICT_TABLE completed, output written to ') +
                       pt_out + '\n' + compress(rows_to_text(pt_res), 2000), sql: pt_sql };
  } catch (e) {
    return { ok: false, response: t('ML_PREDICT_TABLE 失败：', 'ML_PREDICT_TABLE failed: ') + String(e),
             error: 'ml_predict_table_failed' };
  }
}

/* -------------------------------------------------------------- ml_explain */
register_tool({
  name: 'ml_explain', order: 123, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '模型级别解释（特征重要性），model_explainer 可选 ' +
             'permutation_importance|fast_shap|shap|partial_dependence',
         en: 'Model-level feature importance — model_explainer: ' +
             'permutation_importance|fast_shap|shap|partial_dependence' },
  example: { table_name: 'db.table', target_column: 'label', model_handle: 'my_model',
             options: { model_explainer: 'fast_shap' } },
  args: { type: 'object', required: ['table_name', 'model_handle'],
          properties: { table_name:    { type: 'string', minLength: 3 },
                        target_column: { type: 'string' },
                        model_handle:  { type: 'string' },
                        options:       { type: 'object' } } },
  messages: {
    'table_name#required':   { zh: 'ml_explain 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain missing table_name argument (schema.table format)' },
    'table_name#type':       { zh: 'ml_explain 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain missing table_name argument (schema.table format)' },
    'table_name#minLength':  { zh: 'ml_explain 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain missing table_name argument (schema.table format)' },
    'model_handle#required': { zh: 'ml_explain 缺少 model_handle 参数', en: 'ml_explain missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_explain 缺少 model_handle 参数', en: 'ml_explain missing model_handle argument' }
  },
  handler: impl_ml_explain
});

function impl_ml_explain(args, ctx) {
  var ex_table  = esc(String(args.table_name || ''));
  var ex_target = esc(String(args.target_column || ''));
  var ex_handle = esc(String(args.model_handle || ''));
  var ex_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  var ex_sql    = "CALL sys.ML_EXPLAIN('" + ex_table + "', '" + ex_target + "', '" +
                  ex_handle + "', " + ex_opt + ")";
  try {
    var ex_res = query_checked(ex_sql);
    return { ok: true, response: t('ML_EXPLAIN 执行完成。\n', 'ML_EXPLAIN completed.\n') +
             compress(rows_to_text(ex_res), 3000), sql: ex_sql };
  } catch (e) {
    return { ok: false, response: t('ML_EXPLAIN 失败：', 'ML_EXPLAIN failed: ') + String(e),
             error: 'ml_explain_failed' };
  }
}

/* ---------------------------------------------------------- ml_explain_row */
register_tool({
  name: 'ml_explain_row', order: 124, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '解释单行预测', en: 'Explain a single row prediction' },
  example: { model_handle: 'my_model', data: { col1: 'val1' } },
  args: { type: 'object', required: ['model_handle', 'data'],
          properties: { model_handle: { type: 'string' },
                        data:         { type: 'object' },
                        options:      { type: 'object' } } },
  messages: {
    'model_handle#required': { zh: 'ml_explain_row 缺少 model_handle 参数', en: 'ml_explain_row missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_explain_row 缺少 model_handle 参数', en: 'ml_explain_row missing model_handle argument' },
    'data#required':         { zh: 'ml_explain_row 缺少 data 参数（JSON 对象）', en: 'ml_explain_row missing data argument (JSON object)' },
    'data#type':             { zh: 'ml_explain_row 缺少 data 参数（JSON 对象）', en: 'ml_explain_row missing data argument (JSON object)' }
  },
  handler: impl_ml_explain_row
});

function impl_ml_explain_row(args, ctx) {
  var er_handle = esc(String(args.model_handle || ''));
  var er_data   = JSON.stringify(args.data || {});
  var er_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  var er_sql    = "SELECT sys.ML_EXPLAIN_ROW(CAST('" + esc(er_data) + "' AS JSON), '" +
                  er_handle + "', " + er_opt + ") AS explanation";
  try {
    var er_res = query_checked(er_sql);
    return { ok: true, response: t('预测解释结果：\n', 'Prediction explanation:\n') +
             compress(rows_to_text(er_res), 3000), sql: er_sql };
  } catch (e) {
    return { ok: false, response: t('ML_EXPLAIN_ROW 失败：', 'ML_EXPLAIN_ROW failed: ') + String(e),
             error: 'ml_explain_row_failed' };
  }
}

/* -------------------------------------------------------- ml_explain_table */
register_tool({
  name: 'ml_explain_table', order: 125, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_table', fallbackArg: 'table_name', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '整表预测解释，output_table 可省略',
         en: 'Explain predictions on a full table — output_table is optional' },
  example: { table_name: 'db.test', model_handle: 'my_model', output_table: 'db.explain' },
  args: { type: 'object', required: ['table_name', 'model_handle'],
          properties: { table_name:   { type: 'string', minLength: 3 },
                        model_handle: { type: 'string' },
                        output_table: { type: 'string' },
                        options:      { type: 'object' } } },
  messages: {
    'table_name#required':   { zh: 'ml_explain_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain_table missing table_name argument (schema.table format)' },
    'table_name#type':       { zh: 'ml_explain_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain_table missing table_name argument (schema.table format)' },
    'table_name#minLength':  { zh: 'ml_explain_table 缺少 table_name 参数（schema.table 格式）',
                               en: 'ml_explain_table missing table_name argument (schema.table format)' },
    'model_handle#required': { zh: 'ml_explain_table 缺少 model_handle 参数', en: 'ml_explain_table missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_explain_table 缺少 model_handle 参数', en: 'ml_explain_table missing model_handle argument' }
  },
  handler: impl_ml_explain_table
});

function impl_ml_explain_table(args, ctx) {
  var et_table  = esc(String(args.table_name || ''));
  var et_handle = esc(String(args.model_handle || ''));
  var et_out    = args.output_table ? esc(String(args.output_table)) : '';
  var et_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  if (!et_out) {
    var et_parts  = et_table.split('.');
    var et_suffix = '_explain_' + String(Date.now() % 100000);
    et_out = (et_parts.length > 1 ? et_parts[0] + '.' : '') +
             et_parts[et_parts.length - 1] + et_suffix;
  }
  var et_sql = "CALL sys.ML_EXPLAIN_TABLE('" + et_table + "', '" +
               et_handle + "', '" + et_out + "', " + et_opt + ")";
  try {
    var et_res = query_checked(et_sql);
    return { ok: true,
             response: t('ML_EXPLAIN_TABLE 执行完成，结果写入 ',
                         'ML_EXPLAIN_TABLE completed, output written to ') +
                       et_out + '\n' + compress(rows_to_text(et_res), 2000), sql: et_sql };
  } catch (e) {
    return { ok: false, response: t('ML_EXPLAIN_TABLE 失败：', 'ML_EXPLAIN_TABLE failed: ') + String(e),
             error: 'ml_explain_table_failed' };
  }
}

/* ---------------------------------------------------------------- ml_score */
register_tool({
  name: 'ml_score', order: 126, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '评估模型质量。metric: accuracy|balanced_accuracy|f1|precision|recall|roc_auc|neg_log_loss',
         en: 'Evaluate model quality. metric: accuracy|balanced_accuracy|f1|precision|recall|roc_auc|neg_log_loss' },
  example: { table_name: 'db.test', target_column: 'label', model_handle: 'my_model',
             metric: 'balanced_accuracy' },
  args: { type: 'object', required: ['table_name', 'target_column', 'model_handle'],
          properties: { table_name:    { type: 'string', minLength: 3 },
                        target_column: { type: 'string' },
                        model_handle:  { type: 'string' },
                        metric:        { type: 'string' },
                        options:       { type: 'object' } } },
  messages: {
    'table_name#required':    { zh: 'ml_score 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_score missing table_name argument (schema.table format)' },
    'table_name#type':        { zh: 'ml_score 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_score missing table_name argument (schema.table format)' },
    'table_name#minLength':   { zh: 'ml_score 缺少 table_name 参数（schema.table 格式）',
                                en: 'ml_score missing table_name argument (schema.table format)' },
    'target_column#required': { zh: 'ml_score 缺少 target_column 参数', en: 'ml_score missing target_column argument' },
    'target_column#type':     { zh: 'ml_score 缺少 target_column 参数', en: 'ml_score missing target_column argument' },
    'model_handle#required':  { zh: 'ml_score 缺少 model_handle 参数', en: 'ml_score missing model_handle argument' },
    'model_handle#type':      { zh: 'ml_score 缺少 model_handle 参数', en: 'ml_score missing model_handle argument' }
  },
  handler: impl_ml_score
});

function impl_ml_score(args, ctx) {
  var sc_table  = esc(String(args.table_name || ''));
  var sc_target = esc(String(args.target_column || ''));
  var sc_handle = esc(String(args.model_handle || ''));
  var sc_metric = esc(String(args.metric || 'balanced_accuracy'));
  var sc_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
  /* Use a session variable for the OUT parameter.  Execute statements
     separately — see ml_train for rationale. */
  var sc_set_sql  = "SET @_ml_score_val = 0";
  var sc_call_sql = "CALL sys.ML_SCORE('" + sc_table + "', '" + sc_target + "', '" +
                    sc_handle + "', '" + sc_metric + "', @_ml_score_val, " + sc_opt + ")";
  var sc_sel_sql  = "SELECT @_ml_score_val AS score";
  try {
    query_checked(sc_set_sql);
    query_checked(sc_call_sql);
    var sc_res = query_checked(sc_sel_sql);
    var sc_sql_log = sc_set_sql + ";\n" + sc_call_sql + ";\n" + sc_sel_sql;
    return { ok: true, response: t('ML_SCORE 结果：\n', 'ML_SCORE result:\n') +
             compress(rows_to_text(sc_res), 2000), sql: sc_sql_log };
  } catch (e) {
    return { ok: false, response: t('ML_SCORE 失败：', 'ML_SCORE failed: ') + String(e),
             error: 'ml_score_failed' };
  }
}

/* -------------------------------------------------------- ml_model_export */
register_tool({
  name: 'ml_model_export', order: 127, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_table', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '导出模型到表，output_table 可省略', en: 'Export model to table — output_table is optional' },
  example: { model_handle: 'my_model', output_table: 'db.export' },
  args: { type: 'object', required: ['model_handle'],
          properties: { model_handle: { type: 'string' },
                        output_table: { type: 'string' } } },
  messages: {
    'model_handle#required': { zh: 'ml_model_export 缺少 model_handle 参数', en: 'ml_model_export missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_model_export 缺少 model_handle 参数', en: 'ml_model_export missing model_handle argument' }
  },
  handler: impl_ml_model_export
});

function impl_ml_model_export(args, ctx) {
  var me_handle = esc(String(args.model_handle || ''));
  var me_out    = args.output_table ? esc(String(args.output_table)) : '';
  if (!me_out) me_out = 'ml_export_' + String(Date.now() % 100000);
  var me_sql = "CALL sys.ML_MODEL_EXPORT('" + me_handle + "', '" + me_out + "')";
  try {
    var me_res = query_checked(me_sql);
    return { ok: true,
             response: t('ML_MODEL_EXPORT 完成，导出到 ',
                         'ML_MODEL_EXPORT completed, exported to ') +
                       me_out + '\n' + compress(rows_to_text(me_res), 2000), sql: me_sql };
  } catch (e) {
    return { ok: false, response: t('ML_MODEL_EXPORT 失败：', 'ML_MODEL_EXPORT failed: ') + String(e),
             error: 'ml_model_export_failed' };
  }
}

/* -------------------------------------------------------- ml_model_import */
register_tool({
  name: 'ml_model_import', order: 128, category: 'ml',
  write: true, ddl: false, risk: 'high',
  tableArg: 'model_handle', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '导入已导出的模型，model_content 为 export 的表名',
         en: 'Import a previously exported model — model_content is the export table name' },
  example: { model_handle: 'new_model', model_content: 'db.exported', task: 'classification' },
  args: { type: 'object', required: ['model_handle'],
          properties: { model_handle:  { type: 'string' },
                        model_content: { type: 'string' },
                        task:          { type: 'string' } } },
  messages: {
    'model_handle#required': { zh: 'ml_model_import 缺少 model_handle 参数', en: 'ml_model_import missing model_handle argument' },
    'model_handle#type':     { zh: 'ml_model_import 缺少 model_handle 参数', en: 'ml_model_import missing model_handle argument' }
  },
  handler: impl_ml_model_import
});

function impl_ml_model_import(args, ctx) {
  var mi_handle      = esc(String(args.model_handle || ''));
  var mi_content_raw = String(args.model_content || '');
  var mi_task        = String(args.task || 'classification');
  var mi_meta        = JSON.stringify({ task: mi_task });

  if (!mi_content_raw) {
    return { ok: false,
             response: t('ML_MODEL_IMPORT 需要 model_content 参数（从 ml_model_export 导出的表名）。',
                         'ML_MODEL_IMPORT requires model_content parameter (table name from ml_model_export).'),
             error: 'ml_model_import_missing_content' };
  }
  var mi_ident = esc_qualified_ident(mi_content_raw);
  if (!mi_ident) {
    return { ok: false,
             response: t('ML_MODEL_IMPORT 的 model_content 必须是合法表名（schema.table），已拒绝执行。',
                         'ML_MODEL_IMPORT model_content must be a valid schema.table identifier; refusing to execute.'),
             error: 'ml_model_import_invalid_content' };
  }
  var mi_sql = "CALL sys.ML_MODEL_IMPORT((SELECT MODEL_OBJECT FROM " + mi_ident +
               " LIMIT 1), CAST('" + esc(mi_meta) + "' AS JSON), '" + mi_handle + "')";
  try {
    var mi_res = query_checked(mi_sql);
    return { ok: true, response: t('ML_MODEL_IMPORT 完成。\n', 'ML_MODEL_IMPORT completed.\n') +
             compress(rows_to_text(mi_res), 2000), sql: mi_sql };
  } catch (e) {
    return { ok: false, response: t('ML_MODEL_IMPORT 失败：', 'ML_MODEL_IMPORT failed: ') + String(e),
             error: 'ml_model_import_failed' };
  }
}

/* ------------------------------------------ ml_model_load / ml_model_unload
 * ML_PREDICT_* / ML_EXPLAIN_* operate on a model that is resident in memory,
 * so a freshly trained-and-forgotten or re-imported handle has to be loaded
 * before it can serve predictions. */
register_tool({
  name: 'ml_model_load', order: 129, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '将模型加载到内存。ml_predict_* / ml_explain_* 依赖已加载的模型；' +
             '若预测报错提示模型未加载，先调用本工具再重试',
         en: 'Load a model into memory. ml_predict_* / ml_explain_* need a loaded model; ' +
             'if a prediction fails saying the model is not loaded, call this first and retry' },
  example: { model_handle: 'my_model' },
  /* model_handle may be omitted when @chat_options.handle_model is set; the
   * handler resolves it and fails there if neither is present. */
  args: { type: 'object',
          properties: { model_handle: { type: 'string' }, user: { type: 'string' } } },
  messages: {
    'model_handle#type': { zh: 'ml_model_load 的 model_handle 必须是字符串',
                           en: 'ml_model_load model_handle must be a string' }
  },
  handler: impl_ml_model_lifecycle
});

register_tool({
  name: 'ml_model_unload', order: 130, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '将模型从内存卸载，释放内存', en: 'Unload a model from memory to reclaim it' },
  example: { model_handle: 'my_model' },
  args: { type: 'object', properties: { model_handle: { type: 'string' } } },
  messages: {
    'model_handle#type': { zh: 'ml_model_unload 的 model_handle 必须是字符串',
                           en: 'ml_model_unload model_handle must be a string' }
  },
  handler: impl_ml_model_lifecycle
});

function impl_ml_model_lifecycle(args, ctx) {
  var tool = ctx.tool;
  var lc_chat_opt = ctx.chat_opt || get_chat_options();
  var lc_handle = String(args.model_handle ||
                  (lc_chat_opt && lc_chat_opt.handle_model ? lc_chat_opt.handle_model : '') || '');
  if (!lc_handle)
    return { ok: false,
             response: t('缺少 model_handle（也未在 @chat_options.handle_model 中预设）。',
                         'Missing model_handle (and none preset in @chat_options.handle_model).'),
             error: 'ml_model_handle_missing' };
  var lc_sql;
  if (tool === 'ml_model_load') {
    var lc_user = args.user ? "'" + esc(String(args.user)) + "'" : 'NULL';
    lc_sql = "CALL sys.ML_MODEL_LOAD('" + esc(lc_handle) + "', " + lc_user + ")";
  } else {
    lc_sql = "CALL sys.ML_MODEL_UNLOAD('" + esc(lc_handle) + "')";
  }
  try {
    query_checked(lc_sql);
    return { ok: true,
             response: (tool === 'ml_model_load'
               ? t('模型已加载到内存：', 'Model loaded into memory: ')
               : t('模型已从内存卸载：', 'Model unloaded from memory: ')) + lc_handle,
             sql: lc_sql, model_handle: lc_handle };
  } catch (e) {
    return { ok: false,
             response: (tool === 'ml_model_load' ? 'ML_MODEL_LOAD' : 'ML_MODEL_UNLOAD') +
                       t(' 失败：', ' failed: ') + String(e),
             error: tool + '_failed' };
  }
}

/* -------------------------------------------------------- ml_model_active */
register_tool({
  name: 'ml_model_active', order: 131, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '查看当前内存中已加载的模型及占用内存。user 可为 current（默认）或 all',
         en: 'Show which models are resident in memory and how much they use. user: current (default) or all' },
  example: { user: 'current' },
  args: { type: 'object', properties: { user: { type: 'string' } } },
  /* Case-insensitive on purpose: the old switch lower-cased before the
   * membership test, so "Current" was accepted. */
  validate: function(args) {
    if (args.user !== undefined &&
        ['current', 'all'].indexOf(String(args.user).toLowerCase()) === -1)
      return t('ml_model_active 的 user 只能是 current 或 all',
               'ml_model_active user must be either current or all');
    return null;
  },
  handler: impl_ml_model_active
});

function impl_ml_model_active(args, ctx) {
  var ac_user = args.user ? String(args.user).toLowerCase() : 'current';
  var ac_set  = "SET @_ml_active_info = NULL";
  var ac_call = "CALL sys.ML_MODEL_ACTIVE('" + esc(ac_user) + "', @_ml_active_info)";
  var ac_sel  = "SELECT @_ml_active_info AS active_models";
  try {
    query_checked(ac_set);
    query_checked(ac_call);
    var ac_res = query_checked(ac_sel);
    return { ok: true,
             response: t('内存中已加载的模型：\n', 'Models currently loaded in memory:\n') +
                       compress(rows_to_text(ac_res), 2000),
             sql: ac_set + ";\n" + ac_call + ";\n" + ac_sel };
  } catch (e) {
    return { ok: false,
             response: t('ML_MODEL_ACTIVE 失败：', 'ML_MODEL_ACTIVE failed: ') + String(e),
             error: 'ml_model_active_failed' };
  }
}

/* --------------------------------------------------------- ml_list_models */
register_tool({
  name: 'ml_list_models', order: 132, category: 'ml',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['ml'],
  doc: { zh: '列出当前用户所有已训练模型（磁盘上的模型目录）',
         en: 'List all trained models for the current user (the on-disk model catalog)' },
  example: {},
  args: { type: 'object', properties: {} },
  handler: impl_ml_list_models
});

function impl_ml_list_models(args, ctx) {
  try {
    var lm_db = String(ctx.db || '').replace(/@.*$/, '');
    if (!valid_ident(lm_db)) {
      return { ok: false,
               response: t('无法列出模型：当前库名非法。', 'Cannot list models: invalid current database name.'),
               error: 'ml_list_models_invalid_db' };
    }
    var lm_rows = query_checked(
      "SELECT MODEL_HANDLE, TASK, TARGET_COLUMN_NAME, TRAIN_TABLE_NAME, " +
      "MODEL_OBJECT_SIZE, BUILD_TIMESTAMP " +
      "FROM `ML_SCHEMA_" + lm_db + "`.MODEL_CATALOG " +
      "ORDER BY BUILD_TIMESTAMP DESC LIMIT 20"
    );
    return { ok: true, response: t('已训练模型列表：\n', 'Trained models:\n') +
             compress(rows_to_text(lm_rows), 2000) };
  } catch (e) {
    return { ok: false,
             response: t('无法列出模型：', 'Cannot list models: ') + String(e),
             error: 'ml_list_models_failed' };
  }
}

/* ------------------------ ml_embed_table / ml_generate_table / ml_rag_table
 * Batch routines over a whole table column.  ml_embed_table is what actually
 * builds the vector store the RAG route searches, so it is the entry point
 * for "index this text column as a knowledge base".
 *
 * They write their results into an output table column, so they must go
 * through the same approval gate as any other write rather than being
 * mistaken for read-only because args carries no `sql`. */
function validate_batch_column_tool(args, policy, spec) {
  if (!valid_table_column_ref(args.input_column))
    return t(spec.name + ' 的 input_column 必须是 DBName.TableName.ColumnName 格式',
             spec.name + ' input_column must be in DBName.TableName.ColumnName format');
  if (!valid_table_column_ref(args.output_column))
    return t(spec.name + ' 的 output_column 必须是 DBName.TableName.ColumnName 格式',
             spec.name + ' output_column must be in DBName.TableName.ColumnName format');
  if (args.options !== undefined &&
      (typeof args.options !== 'object' || Array.isArray(args.options)))
    return t(spec.name + ' 的 options 必须是 JSON 对象',
             spec.name + ' options must be a JSON object');
  return null;
}

register_tool({
  name: 'ml_embed_table', order: 133, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_column', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '【批量向量化】把一列文本编码为向量写入另一列，这是构建 RAG 知识库（向量库）的入口\n' +
             '可选 args.options：{"model_id":"...","batch_size":500,"truncate":true}',
         en: '[Batch embedding] Encode a text column into vectors in another column — this is how a ' +
             'RAG knowledge base (vector store) gets built\n' +
             'Optional args.options: {"model_id":"...","batch_size":500,"truncate":true}' },
  example: { input_column: 'db.docs.content', output_column: 'db.docs.segment_embedding' },
  args: { type: 'object', required: ['input_column', 'output_column'],
          properties: { input_column:  { type: 'string' },
                        output_column: { type: 'string' },
                        options:       { type: 'object' } } },
  validate: validate_batch_column_tool,
  handler:  impl_batch_column_tool
});

register_tool({
  name: 'ml_generate_table', order: 134, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_column', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '【批量生成】对整列文本批量调用 LLM（如批量摘要/分类/改写）\n' +
             '可选 args.options：{"task":"summarization","model_id":"...","context_column":"..."}',
         en: '[Batch generation] Run the LLM over a whole column (bulk summarize / classify / rewrite)\n' +
             'Optional args.options: {"task":"summarization","model_id":"...","context_column":"..."}' },
  example: { input_column: 'db.t.prompt', output_column: 'db.t.answer' },
  args: { type: 'object', required: ['input_column', 'output_column'],
          properties: { input_column:  { type: 'string' },
                        output_column: { type: 'string' },
                        options:       { type: 'object' } } },
  validate: validate_batch_column_tool,
  handler:  impl_batch_column_tool
});

register_tool({
  name: 'ml_rag_table', order: 135, category: 'ml',
  write: true, ddl: false, risk: 'medium',
  tableArg: 'output_column', transactional: false,
  capabilities: ['ml'],
  doc: { zh: '【批量 RAG】对整列问题批量检索知识库并生成回答',
         en: '[Batch RAG] Answer a whole column of questions against the knowledge base' },
  example: { input_column: 'db.q.question', output_column: 'db.q.answer' },
  args: { type: 'object', required: ['input_column', 'output_column'],
          properties: { input_column:  { type: 'string' },
                        output_column: { type: 'string' },
                        options:       { type: 'object' } } },
  validate: validate_batch_column_tool,
  handler:  impl_batch_column_tool
});

function impl_batch_column_tool(args, ctx) {
  var tool   = ctx.tool;
  var bt_in  = String(args.input_column  || '');
  var bt_out = String(args.output_column || '');
  if (!valid_table_column_ref(bt_in) || !valid_table_column_ref(bt_out))
    return { ok: false,
             response: t('input_column / output_column 必须是 DBName.TableName.ColumnName 格式。',
                         'input_column / output_column must be DBName.TableName.ColumnName.'),
             error: 'invalid_table_column_ref' };

  /* Copy rather than mutate the caller's args — the same object is stored
   * verbatim in the review plan's args_json. */
  var bt_opts = {};
  if (args.options && typeof args.options === 'object')
    Object.keys(args.options).forEach(function(k) { bt_opts[k] = args.options[k]; });

  var bt_proc;
  if (tool === 'ml_embed_table') {
    bt_proc = 'sys.ML_EMBED_TABLE';
    if (!bt_opts.model_id) bt_opts.model_id = get_embed_model_id();
    /* ML_EMBED_TABLE reads this as CAST(JSON_UNQUOTE(...) AS UNSIGNED), so a
     * JSON boolean unquotes to 'true' and fails with ER_TRUNCATED_WRONG_VALUE.
     * It wants 1/0.  (ML_EMBED_ROW, used elsewhere, does accept true.) */
    if (bt_opts.truncate === undefined) bt_opts.truncate = 1;
    else bt_opts.truncate = bt_opts.truncate ? 1 : 0;
  } else if (tool === 'ml_generate_table') {
    bt_proc = 'sys.ML_GENERATE_TABLE';
    var gen_model_opts = (ctx.chat_opt || get_chat_options() || {}).model_options || {};
    if (!bt_opts.task)     bt_opts.task = 'generation';
    if (!bt_opts.language) bt_opts.language = A.lang;
    if (!bt_opts.model_id && gen_model_opts.model_id)
      bt_opts.model_id = gen_model_opts.model_id;
  } else {
    bt_proc = 'sys.ML_RAG_TABLE';
    var rag_defaults = get_rag_options(ctx.chat_opt || get_chat_options());
    Object.keys(rag_defaults).forEach(function(k) {
      if (!(k in bt_opts)) bt_opts[k] = rag_defaults[k];
    });
  }

  var bt_sql = "CALL " + bt_proc + "('" + esc(bt_in) + "','" + esc(bt_out) + "'," +
               "CAST('" + esc(JSON.stringify(bt_opts)) + "' AS JSON))";
  try {
    var bt_res = query_checked(bt_sql);
    return { ok: true,
             response: bt_proc.replace('sys.', '') +
                       t(' 执行完成，结果写入 ', ' completed, output written to ') + bt_out +
                       '\n' + compress(rows_to_text(bt_res), 2000),
             sql: bt_sql };
  } catch (e) {
    return { ok: false,
             response: bt_proc.replace('sys.', '') + t(' 失败：', ' failed: ') + String(e),
             error: tool + '_failed' };
  }
}
