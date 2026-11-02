#!/usr/bin/env python3
"""Compare two Rapid binaries on the same TPC-H data; retain timings, plans and result hashes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import shutil
import subprocess
import time


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--client', required=True, type=Path)
    parser.add_argument('--data', required=True, type=Path)
    parser.add_argument('--queries', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--database', default='tpch')
    parser.add_argument('--rounds', default=3, type=int)
    parser.add_argument('--main-ref', default='unspecified')
    parser.add_argument('--candidate-ref', default='unspecified')
    parser.add_argument('--port', default=14750, type=int)
    parser.add_argument('--main-workers', default=1, type=int, choices=range(1, 33), help='Main session parallel worker limit')
    parser.add_argument('--candidate-workers', default=1, type=int, choices=range(1, 33), help='Candidate session parallel worker limit')
    parser.add_argument('--reverse', action='store_true', help='Run candidate before main for order confirmation')
    parser.add_argument('--preflight', action='store_true', help='Candidate correctness only; timings are not performance evidence')
    parser.add_argument('--candidate-only', action='store_true', help='Reuse a separately preserved main baseline')
    parser.add_argument('--ready-file', type=Path, help='Wait for verification to finish before warmup/timing')
    parser.add_argument('--query-numbers', nargs='+', type=int, default=list(range(1, 23)))
    parser.add_argument('--kernel-probes', action='store_true', help='Also compare encoded predicates, extrema and deferred projection at the selected scale')
    args = parser.parse_args()
    variants = [('candidate', args.candidate)] if args.preflight or args.candidate_only else [('main', args.main), ('candidate', args.candidate)]
    output = args.output.resolve()
    if args.rounds < 1:
        parser.error('--rounds must be positive')
    if output.exists() and any(output.iterdir()):
        parser.error('--output must be empty to preserve earlier evidence')
    output.mkdir(parents=True, exist_ok=True)
    worker_limits = dict(main=args.main_workers, candidate=args.candidate_workers)
    worker_preambles = {}
    socket = args.data.parent / 'benchmark.sock'
    records = []
    sql_queries = {q: (args.queries / f'q{q}.sql').read_text() for q in args.query_numbers}
    if args.kernel_probes:
        probes = {
            100: "SELECT COUNT(*) FROM PART;",
            101: "SELECT COUNT(*) FROM PART WHERE P_BRAND='Brand#35';",
            102: "SELECT COUNT(*) FROM PART WHERE P_SIZE>=30 AND P_SIZE<40;",
            103: "SELECT MIN(P_BRAND),MAX(P_TYPE) FROM PART;",
            104: "SELECT SUM(P_RETAILPRICE*2),SUM(P_RETAILPRICE+P_SIZE),SUM(P_RETAILPRICE-P_SIZE) FROM PART;",
            105: "SELECT SUM(P_PARTKEY),SUM(P_RETAILPRICE) FROM PART WHERE LENGTH(P_COMMENT)<10;",
            106: "SELECT SUM(P_PARTKEY),SUM(P_RETAILPRICE) FROM PART WHERE LENGTH(P_COMMENT)<15;",
            107: "SELECT SUM(P_PARTKEY),SUM(P_RETAILPRICE) FROM PART WHERE LENGTH(P_COMMENT)<20;",
            108: "SELECT COUNT(*),SUM(P_PARTKEY) FROM PART WHERE P_TYPE NOT LIKE 'MEDIUM POLISHED%' AND LENGTH(P_COMMENT)<15;",
            109: "SELECT P_SIZE,MIN(P_BRAND),MAX(P_TYPE) FROM PART GROUP BY P_SIZE;",
            110: "SELECT COUNT(*) FROM PART WHERE P_BRAND<>'Brand#35';",
            111: "SELECT COUNT(*) FROM PART WHERE P_COMMENT LIKE '%special%';",
        }
        sql_queries.update(probes)
        args.query_numbers.extend(probes)
        for number, sql in probes.items():
            (output / f'probe{number}.sql').write_text(sql + '\n')
    metadata = dict(database=args.database, rounds=args.rounds, reverse=args.reverse,
                    source_refs=dict(main=args.main_ref, candidate=args.candidate_ref),
                    parallel_workers=worker_limits,
                    client=dict(path=str(args.client.resolve()), sha256=sha256(args.client)),
                    queries={q: hashlib.sha256(sql.encode()).hexdigest() for q, sql in sql_queries.items()},
                    binaries={side: dict(path=str(binary.resolve()), sha256=sha256(binary))
                              for side, binary in variants},
                    clock='client perf_counter; server NOW(6) timestamps around query', preflight=args.preflight,
                    data=str(args.data.resolve()), cpu=os.cpu_count())
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    for side, binary in variants:
        shutil.copyfile(binary.resolve().parent.parent / 'CMakeCache.txt', output / f'{side}.CMakeCache.txt')

    def runtime_sample():
        vmstat = dict(line.split() for line in Path('/proc/vmstat').read_text().splitlines())
        return dict(loadavg=list(os.getloadavg()), swap_in=int(vmstat['pswpin']), swap_out=int(vmstat['pswpout']))

    def client(sql, database=True, timeout=240):
        command = [str(args.client.resolve()), '--no-defaults', '--protocol=SOCKET',
                   f'--socket={socket}', '-uroot', '--batch', '--raw', '--skip-column-names',
                   '--default-character-set=utf8mb4', '--skip-reconnect']
        if database:
            command.append(args.database)
        return subprocess.run(command, input=sql, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)

    def run_query(side, q, round_number, innodb=False):
        folder = output / side / ('reference' if innodb else f'round{round_number}')
        folder.mkdir(parents=True, exist_ok=True)
        preamble = ("SET SESSION max_execution_time=180000; SET SESSION time_zone='+00:00'; "
                    f"SET SESSION optimizer_switch='hypergraph_optimizer={'off' if innodb else 'on'}'; "
                    f"SET SESSION use_secondary_engine={'OFF' if innodb else 'FORCED'};\n")
        preamble += worker_preambles[side]
        status = ("SHOW SESSION STATUS LIKE 'Handler_read%';\n"
                  "SHOW GLOBAL STATUS LIKE 'rapid_query%';\n")
        sql = (preamble + "SELECT 'BENCH_BEFORE';\n" + status +
               "SELECT 'BENCH_RESULT';\nSET @bench_start=NOW(6);\n" + sql_queries[q] +
               "\nSET @bench_us=TIMESTAMPDIFF(MICROSECOND,@bench_start,NOW(6));\n"
               "SELECT 'BENCH_AFTER',@bench_us;\n" + status)
        runtime_before = runtime_sample()
        start = time.perf_counter()
        try:
            result = client(sql)
            elapsed = time.perf_counter() - start
            stdout, stderr, rc = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            elapsed = time.perf_counter() - start
            stdout = error.stdout or b''
            stderr = error.stderr or b''
            if isinstance(stdout, bytes): stdout = stdout.decode(errors='replace')
            if isinstance(stderr, bytes): stderr = stderr.decode(errors='replace')
            rc = 124
        (folder / f'q{q}.raw.tsv').write_text(stdout)
        (folder / f'q{q}.stderr').write_text(stderr)
        record = dict(side=side, query=q, round=round_number, reference=innodb,
                      client_seconds=elapsed, rc=rc, runtime_before=runtime_before, runtime_after=runtime_sample())
        before, result_rows, after = {}, [], {}
        phase = None
        for line in stdout.splitlines():
            if line == 'BENCH_BEFORE': phase = 'before'; continue
            if line == 'BENCH_RESULT': phase = 'result'; continue
            if line.startswith('BENCH_AFTER\t'):
                record['server_seconds'] = int(line.split('\t')[1]) / 1e6
                phase = 'after'; continue
            if phase == 'result': result_rows.append(line)
            elif phase in ('before', 'after'):
                name, value = line.split('\t', 1)
                (before if phase == 'before' else after)[name] = int(value)
        record['counters'] = {name: value - before.get(name, 0) for name, value in after.items()
                              if not name.endswith('_max')}
        record['global_maxima'] = {name: dict(before=before.get(name), after=value)
                                  for name, value in after.items() if name.endswith('_max')}
        if q >= 100 and 'server_seconds' in record:
            record['scan_query_ns_per_input_row'] = record['server_seconds'] * 1e9 / part_rows
        record['rows'] = len(result_rows)
        record['result_sha256'] = hashlib.sha256('\n'.join(sorted(result_rows)).encode()).hexdigest()
        record['ordered_result_sha256'] = hashlib.sha256('\n'.join(result_rows).encode()).hexdigest()
        (folder / f'q{q}.result.tsv').write_text('\n'.join(result_rows) + ('\n' if result_rows else ''))
        if rc == 0 and 'server_seconds' not in record:
            raise RuntimeError(f'Missing timing marker for {side} q{q}')
        records.append(record)
        (output / 'timings.json').write_text(json.dumps(records, indent=2))
        print(json.dumps({k: v for k, v in record.items() if k != 'counters'}), flush=True)
        if rc != 0:
            print(stderr, flush=True)
        if rc == 124:
            raise RuntimeError(f'Client timeout for {side} q{q}; abort to avoid overlapping queries')
        return record

    if client('SELECT 1;', database=False, timeout=5).returncode == 0:
        raise RuntimeError('Benchmark socket is already served by another process')
    if args.reverse: variants.reverse()
    for side, binary in variants:
        if sha256(binary) != metadata['binaries'][side]['sha256']:
            raise RuntimeError(f'{side} binary changed after metadata capture')
        log = open(output / f'{side}.server-stdout.log', 'w')
        command = [str(binary.resolve()), '--no-defaults', f'--basedir={binary.resolve().parent.parent}',
                   f'--datadir={args.data.resolve()}', f'--socket={socket}', f'--port={args.port}',
                   '--bind-address=127.0.0.1', '--mysqlx=OFF', '--skip-log-bin',
                   f'--pid-file={args.data.parent / "benchmark.pid"}',
                   f'--log-error={output / (side + ".server.log")}',
                   '--innodb-buffer-pool-size=2G', '--rapid-memory-size-max=4G',
                   '--rapid-schema-embedding=OFF', '--rapid-reload-on-restart=OFF',
                   '--secondary-engine-cost-threshold=0', '--secure-file-priv=']
        (output / f'{side}.command.json').write_text(json.dumps(command, indent=2))
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            for _ in range(180):
                if process.poll() is not None:
                    raise RuntimeError(f'{side} server exited: {process.returncode}')
                if client('SELECT 1;', database=False, timeout=5).returncode == 0: break
                time.sleep(1)
            else: raise RuntimeError(f'{side} server startup timeout')
            meta = client('SELECT VERSION(); SHOW VARIABLES;', database=False)
            (output / f'{side}.variables.tsv').write_text(meta.stdout)
            workers_variable = client("SHOW VARIABLES LIKE 'rapid_parallel_workers';", database=False)
            if workers_variable.returncode:
                raise RuntimeError(f'{side} worker configuration lookup failed')
            if workers_variable.stdout.strip():
                # Set even 1 explicitly: persisted server defaults must not change
                # the serial baseline. Older binaries remain usable with limit 1.
                worker_preambles[side] = f"SET SESSION rapid_parallel_workers={worker_limits[side]};\n"
            elif worker_limits[side] != 1:
                raise RuntimeError(f'{side} binary does not support parallel workers')
            else:
                worker_preambles[side] = ''
            load_sql = 'SET use_secondary_engine=OFF;\n' + '\n'.join(
                f'ALTER TABLE {table} SECONDARY_LOAD;' for table in
                ('REGION','NATION','SUPPLIER','CUSTOMER','PART','PARTSUPP','ORDERS','LINEITEM'))
            loaded = client(load_sql, timeout=900)
            (output / f'{side}.load.log').write_text(loaded.stdout + loaded.stderr)
            if loaded.returncode: raise RuntimeError(f'{side} secondary load failed')
            counts = client('SET use_secondary_engine=OFF;\n' + ' UNION ALL '.join(
                f"SELECT '{table}',COUNT(*) FROM {table}" for table in
                ('REGION','NATION','SUPPLIER','CUSTOMER','PART','PARTSUPP','ORDERS','LINEITEM')) + ';')
            (output / f'{side}.table-counts.tsv').write_text(counts.stdout + counts.stderr)
            if counts.returncode: raise RuntimeError(f'{side} table-count verification failed')
            part_rows = next(int(line.split('\t')[1]) for line in counts.stdout.splitlines() if line.startswith('PART\t'))
            if args.database == 'tpch':
                expected = dict(REGION=5, NATION=25, SUPPLIER=10000, CUSTOMER=150000,
                                PART=200000, PARTSUPP=800000, ORDERS=1500000, LINEITEM=6001215)
                actual = {line.split('\t')[0]: int(line.split('\t')[1]) for line in counts.stdout.splitlines()}
                if actual != expected: raise RuntimeError(f'SF=1 counts differ: {actual}')
            worker_sql = worker_preambles[side]
            for q in args.query_numbers:
                plan = client(worker_sql + "SET optimizer_switch='hypergraph_optimizer=on'; SET use_secondary_engine=FORCED;\n"
                              'EXPLAIN FORMAT=TREE ' + sql_queries[q])
                plans = output / side / 'plans'; plans.mkdir(parents=True, exist_ok=True)
                (plans / f'q{q}.tree').write_text(plan.stdout + plan.stderr)
            if args.ready_file:
                print(f'{side}: loaded and planned; waiting for {args.ready_file}', flush=True)
                while not args.ready_file.exists(): time.sleep(1)
            if side == 'main' or args.preflight:
                for q in args.query_numbers: run_query(side, q, -1, innodb=True)
            for round_number in range(1 if args.preflight else args.rounds + 1):
                # Reverse query order on alternating rounds to expose ordering effects.
                order = args.query_numbers if round_number % 2 == 0 else list(reversed(args.query_numbers))
                for q in order: run_query(side, q, round_number)
            if not args.preflight:
                for q in (2, 20, 22):
                    if q not in sql_queries: continue
                    analyzed = client(worker_sql + "SET max_execution_time=180000; SET optimizer_switch='hypergraph_optimizer=on'; "
                                      "SET use_secondary_engine=FORCED;\nEXPLAIN ANALYZE " + sql_queries[q])
                    plans = output / side / 'plans'
                    (plans / f'q{q}.analyze').write_text(analyzed.stdout + analyzed.stderr)
                    (plans / f'q{q}.analyze.rc').write_text(str(analyzed.returncode) + '\n')
        finally:
            if process.poll() is None:
                try: client('SHUTDOWN;', database=False, timeout=30)
                finally:
                    try: process.wait(timeout=60)
                    except subprocess.TimeoutExpired:
                        process.terminate(); process.wait(timeout=30)
            log.close()

    lines = ['| Query | Main median s | Candidate median s | Main/candidate | Results |',
             '|---|---:|---:|---:|---|']
    failed = []
    for q in args.query_numbers:
        values = [r for r in records if r['query'] == q]
        correct = all(r['rc'] == 0 for r in values) and len({r['result_sha256'] for r in values}) == 1
        if not correct:
            failed.append(q)
        if args.preflight:
            lines.append(f'| Q{q} | — | — | — | {"identical" if correct else "FAIL"} |')
            continue
        medians = {side: statistics.median(r['server_seconds'] for r in values
                   if r['side'] == side and r['round'] > 0 and r['rc'] == 0)
                   for side in ('main', 'candidate')
                   if any(r['side'] == side and r['round'] > 0 and r['rc'] == 0 for r in values)}
        if len(medians) == 2:
            baseline, candidate = medians['main'], medians['candidate']
            ratio = f'{baseline/candidate:.3f}x' if correct and candidate else '—'
            lines.append(f'| Q{q} | {baseline:.6f} | {candidate:.6f} | {ratio} | {"identical" if correct else "FAIL"} |')
        else: lines.append(f'| Q{q} | — | — | — | ERROR |')
    (output / 'summary.md').write_text('\n'.join(lines) + '\n')
    if failed:
        raise SystemExit(f'Failed execution or result comparison for queries: {failed}')


if __name__ == '__main__':
    main()
