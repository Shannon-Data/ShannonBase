const fs = require('fs');
const vm = require('vm');

// Load lib_tools.js into a VM and provide minimal stubs for DB and system functions
const libCode = fs.readFileSync(__dirname + '/lib_tools.js', 'utf8');

// In-memory simulation of mysql.agent_review_* tables and some application tables
const memory = {
  agent_review_plan: {},
  agent_review_plan_step: {},
  agent_review_history: [],
  agent_rollback_log: [],
  tables: { users: 10, orders: 5 }
};

const sandbox = {
  console: console,
  A: {},
  esc: function(s){ return String(s || '').replace(/'/g, "''"); },
  t: function(a,b){ return b || a; },
  gen_query_id: function(){ return 'plan-' + Math.random().toString(36).slice(2,10); },
  cfg: function(k, d){ return d; },
  detect_lang: function(){ return 'en'; },
  // small helpers used by lib_tools
  compress: function(s, max) { var x = String(s || ''); return (max && x.length>max) ? x.substring(0,max) : x; },
  rows_to_text: function(rows) { try { return JSON.stringify(rows); } catch(e){ return String(rows); } },
  query: function(sql){
    sql = String(sql || '').trim();
    // information_schema TABLES count lookup
    if (/FROM information_schema.TABLES/i.test(sql) && /TABLE_NAME IN \(/i.test(sql)) {
      var m = sql.match(/IN \(([^)]+)\)/i);
      var names = [];
      if (m) names = m[1].split(',').map(function(s){ return s.replace(/['"]+/g,'').trim(); });
      return names.map(function(n){ return { TABLE_NAME: n.replace(/'/g,''), TABLE_ROWS: memory.tables[n.replace(/'/g,'')] || 0 }; });
    }
    // select from mysql.agent_review_plan (avoid matching agent_review_plan_step)
    if (/FROM\s+mysql\.agent_review_plan\b/i.test(sql)) {
      var convMatch = sql.match(/conversation_id\s*=\s*'([^']+)'/i);
      var conv = convMatch ? convMatch[1] : null;
      var res = [];
      for (var k in memory.agent_review_plan) {
        var p = memory.agent_review_plan[k];
        if (p.conversation_id === conv && p.status === 'awaiting_approval') res.push(p);
      }
      // order by created_at desc simulated by insertion order; return latest
      if (res.length) return [res[res.length-1]];
      return [];
    }
    // select steps
    if (/FROM mysql\.agent_review_plan_step/i.test(sql)) {
      var planMatch = sql.match(/plan_id\s*=\s*'([^']+)'/i);
      var pid = planMatch ? planMatch[1] : null;
      console.log('QUERY STEPS for plan_id=', pid, 'stored keys=', Object.keys(memory.agent_review_plan_step));
      var out = [];
      for (var k in memory.agent_review_plan_step) {
        var s = memory.agent_review_plan_step[k];
        if (s.plan_id === pid) out.push(s);
      }
      out.sort(function(a,b){ return a.step_no - b.step_no; });
      return out;
    }
    return [];
  },
  sys: {
    exec_sql: function(sql){
      sql = String(sql || '').trim();
      if (/agent_review_plan|agent_review_plan_step/i.test(sql)) console.log('SYS.EXEC_SQL:', sql.substring(0,400));
      if (/START TRANSACTION/i.test(sql)) { sandbox._tx = true; return; }
      if (/COMMIT/i.test(sql)) { sandbox._tx = false; return; }
      if (/ROLLBACK/i.test(sql)) { sandbox._tx = false; return; }
      // INSERT INTO mysql.agent_review_plan(... ) VALUES ('pid','conv','status', idx, total, 'desc', 'json') ...
      var m = sql.match(/INSERT INTO mysql\.agent_review_plan\([^)]*\)\s*VALUES\s*\(([^)]+)\)/i);
      if (m) {
        var vals = m[1];
        var parts = vals.split(/\s*,\s*/);
        // crude parse assuming order: plan_id,conversation_id,status,current_step_index,total_steps,description,plan_json
        var plan_id = parts[0].replace(/^'|'$/g,'');
        var conversation_id = parts[1].replace(/^'|'$/g,'');
        var status = parts[2].replace(/^'|'$/g,'');
        var current_step_index = Number(parts[3]);
        var total_steps = Number(parts[4]);
        var description = parts[5].replace(/^'|'$/g,'').replace(/''/g,"'");
        var plan_json = parts[6].replace(/^'|'$/g,'').replace(/''/g,"'");
        memory.agent_review_plan[plan_id] = { plan_id: plan_id, conversation_id: conversation_id, status: status, current_step_index: current_step_index, total_steps: total_steps, description: description, plan_json: plan_json };
        return;
      }
      // DELETE FROM mysql.agent_review_plan_step WHERE plan_id='pid'
      var delm = sql.match(/DELETE FROM mysql\.agent_review_plan_step\s+WHERE\s+plan_id\s*=\s*'([^']+)'/i);
      if (delm) {
        var pid = delm[1];
        for (var k in memory.agent_review_plan_step) if (memory.agent_review_plan_step[k].plan_id === pid) delete memory.agent_review_plan_step[k];
        return;
      }
      // INSERT INTO mysql.agent_review_plan_step(... ) VALUES ('plan', 1, 'tool', 'sql', 'args_json', 'tables', writes, ddl, 'risk', 'est', 'status', 'preview', 'error')
      var m2 = sql.match(/INSERT INTO mysql\.agent_review_plan_step\([^)]*\)\s*VALUES\s*\(([\s\S]+)\)\s*$/i);
      if (m2) {
        // Parse CSV-style values inside parentheses, respecting single quotes
        var vals = m2[1];
        var parts = [];
        var cur = '';
        var inq = false;
        for (var i = 0; i < vals.length; i++) {
          var ch = vals[i];
          if (ch === "'") {
            // handle escaped '' as one '
            if (inq && vals[i+1] === "'") { cur += "'"; i++; continue; }
            inq = !inq; cur += ch; continue;
          }
          if (!inq && ch === ',') { parts.push(cur.trim()); cur = ''; continue; }
          cur += ch;
        }
        if (cur.length) parts.push(cur.trim());
        for (var j = 0; j < parts.length; j++) parts[j] = parts[j].replace(/^'|'$/g,'').replace(/''/g,"'");
        var plan_id = parts[0];
        var step_no = Number(parts[1]);
        var tool = parts[2];
        var sql_text = parts[3];
        var args_json = parts[4];
        var affected_tables = parts[5];
        var writes = Number(parts[6]);
        var ddl = Number(parts[7]);
        var risk = parts[8];
        var estimated_rows = parts[9];
        var status = parts[10];
        memory.agent_review_plan_step[plan_id + ':' + step_no] = { plan_id: plan_id, step_no: step_no, tool: tool, sql_text: sql_text, args_json: args_json, affected_tables: affected_tables, writes: writes, ddl: ddl, risk: risk, estimated_rows: estimated_rows, status: status, result_preview: '', error_text: '' };
        return;
      }
      // INSERT INTO mysql.agent_review_history
      // INSERT INTO ... history / rollback (relaxed match)
      if (/INSERT INTO mysql\.agent_review_history/i.test(sql)) { memory.agent_review_history.push({ raw: sql }); return; }
      if (/INSERT INTO mysql\.agent_rollback_log/i.test(sql)) { memory.agent_rollback_log.push({ raw: sql }); return; }

      // UPDATE mysql.agent_review_plan_step SET ... WHERE plan_id='pid' AND step_no=1
      var upStep = sql.match(/UPDATE\s+mysql\.agent_review_plan_step\s+SET\s+([^\s]+)\s*=\s*'([^']*)'[^W]*WHERE\s+plan_id\s*=\s*'([^']*)'\s+AND\s+step_no\s*=\s*([0-9]+)/i);
      if (upStep) {
        var field = upStep[1], value = upStep[2], pid = upStep[3], stepno = Number(upStep[4]);
        var key = pid + ':' + stepno;
        if (memory.agent_review_plan_step[key]) {
          memory.agent_review_plan_step[key][field === 'status' ? 'status' : 'result_preview'] = value;
        }
        return;
      }
      // UPDATE mysql.agent_review_plan SET status='completed' WHERE plan_id='pid'
      var upPlan = sql.match(/UPDATE\s+mysql\.agent_review_plan\s+SET\s+status\s*=\s*'([^']*)'[^W]*WHERE\s+plan_id\s*=\s*'([^']*)'/i);
      if (upPlan) {
        var st = upPlan[1], pid2 = upPlan[2];
        if (memory.agent_review_plan[pid2]) memory.agent_review_plan[pid2].status = st;
        return;
      }

      // For update_data or other DML statements, attempt to simulate affecting in-memory tables
      var ins = sql.match(/INSERT INTO\s+([a-zA-Z0-9_]+)\s*/i);
      if (ins) {
        var tbl = ins[1];
        if (memory.tables.hasOwnProperty(tbl)) { memory.tables[tbl] += 1; }
        return;
      }
      var upd = sql.match(/UPDATE\s+([a-zA-Z0-9_]+)\s+SET/i);
      if (upd) {
        var tbl = upd[1];
        if (memory.tables.hasOwnProperty(tbl)) { memory.tables[tbl] += 0; }
        return;
      }
    }
  },
  _tx: false
};

// Run the lib code inside this sandbox
vm.createContext(sandbox);
vm.runInContext(libCode, sandbox, { filename: 'lib_tools.js' });

// Now we can call functions from the loaded lib_tools.js via the sandbox
function runTest() {
  const plan_id = sandbox.gen_query_id();
  const conv = 'conv-test-123';
  // Build two steps: a read-only count, then an insert into users
  const steps = [
    {
      id: 1,
      tool: 'query_db',
      sql: 'SELECT COUNT(*) FROM users',
      args: { sql: 'SELECT COUNT(*) FROM users' },
      affected_tables: ['users'],
      writes: false,
      ddl: false,
      risk: 'low'
    },
    {
      id: 2,
      tool: 'update_data',
      sql: "INSERT INTO users (name) VALUES ('new')",
      args: { sql: "INSERT INTO users (name) VALUES ('new')" },
      affected_tables: ['users'],
      writes: true,
      ddl: false,
      risk: 'medium'
    }
  ];

  const review_state = {
    plan_id: plan_id,
    conversation_id: conv,
    status: 'awaiting_approval',
    description: 'Test multi-step plan',
    current_step_index: 0,
    total_steps: steps.length,
    steps: steps
  };

  console.log('--- Saving review plan ---');
  sandbox.save_review_plan(review_state);
  console.log('Plan stored in memory.agent_review_plan keys:', Object.keys(memory.agent_review_plan));
  console.log('Plan step keys:', Object.keys(memory.agent_review_plan_step));
  console.log('Sample step object:', memory.agent_review_plan_step[plan_id + ':1']);

  // Directly query steps to verify stored rows
  var direct = sandbox.query("SELECT step_no, tool, sql_text, args_json, affected_tables, writes, ddl, risk, estimated_rows, status, result_preview, error_text FROM mysql.agent_review_plan_step WHERE plan_id='" + plan_id + "' ORDER BY step_no");
  console.log('Direct query step rows:', JSON.stringify(direct, null, 2));

  console.log('--- Loading review state ---');
  const loaded = sandbox.load_review_state(conv);
  console.log('Loaded:', JSON.stringify(loaded, null, 2));

  console.log('--- Executing step 1 (read-only) ---');
  var step1 = sandbox.get_pending_review_step(loaded);
  var res1 = sandbox.execute_review_step(step1, 'testdb');
  console.log('Result step1:', res1);

  console.log('--- Mark step1 approved and save ---');
  loaded.current_step_index += 1;
  sandbox.update_review_step_status(loaded.plan_id, step1.id, 'approved', res1.response || '');
  sandbox.save_review_plan(loaded);

  console.log('--- Executing step 2 (write) ---');
  var loaded2 = sandbox.load_review_state(conv);
  var step2 = sandbox.get_pending_review_step(loaded2);
  var before = JSON.stringify(memory.tables);
  var res2 = sandbox.execute_review_step(step2, 'testdb');
  var after = JSON.stringify(memory.tables);
  console.log('Before tables:', before);
  console.log('After tables: ', after);
  console.log('Result step2:', res2);

  console.log('--- Append history and finalize ---');
  sandbox.append_review_history(conv, loaded.plan_id, step2.id, 'approve', 'approve', '', res2.response || '', !res2.ok);
  sandbox.clear_review_state(conv, loaded.plan_id, 'completed');

  console.log('agent_review_history length:', memory.agent_review_history.length);
  console.log('agent_review_plan final status:', memory.agent_review_plan[loaded.plan_id] && memory.agent_review_plan[loaded.plan_id].status);
}

runTest();

// Exit when done
process.exit(0);
