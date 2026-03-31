import json
import re
import sys

def main():
    try:
        with open('benchmark_results.json', 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print("benchmark_results.json not found.")
        sys.exit(1)

    results = { r['name']: r for r in data['results'] }

    try:
        with open('DesignDoc.md', 'r') as f:
            content = f.read()
    except FileNotFoundError:
        print("DesignDoc.md not found.")
        sys.exit(1)

    # Helper function to format row replacement
    def replace_row(op_name, match_str, time_fmt, tp_fmt, json_key, analysis_text):
        nonlocal content
        if json_key not in results:
            print(f"Key {json_key} not in JSON")
            return
        
        r = results[json_key]
        if ' s' in time_fmt or ' ms' in time_fmt or ' μs' in time_fmt:
            # Determine time unit
            t = r['elapsed_ms']
            if op_name == 'INSERT 10M rows' or op_name == 'SELECT * (full scan)':
                t_str = f"{t/1000.0:.1f} s"
            elif op_name == 'Point lookup (100×)':
                t_str = f"{t:.2f} ms"
            else:
                t_str = f"{t:.2f} ms"
        
        if op_name == 'Point lookup (100×)':
            # avg time per query
            tp_str = f"{r['elapsed_ms']/r['row_count']*1000.0:.0f} μs avg"
        else:
            tp_str = f"{int(r['rows_per_sec']):,} rows/s"

        pattern = r'\|\s*' + re.escape(op_name) + r'\s*\|[^|]+\|[^|]+\|[^|]+\|'
        replacement = f"| {op_name} | {t_str} | **{tp_str}** | {analysis_text} |"
        
        content = re.sub(pattern, replacement, content)

    analysis_insert = "Includes WAL append per mutation for persistence. Arena allocation is O(1). Batching enabled."
    analysis_select = "Sequential arena access gives excellent cache locality. Zero-cost offset resolution."
    analysis_point = "3-level B+ tree traversal with binary search."
    analysis_range = "Leaf chain traversal — no random access."
    analysis_join = "Zero-alloc hash join (Value keys, no to_string)."

    replace_row('INSERT 10M rows', '', ' s', ' rows/s', 'INSERT 10000000 rows', analysis_insert)
    replace_row('SELECT * (full scan)', '', ' s', ' rows/s', 'SELECT * (full scan)', analysis_select)
    replace_row('Point lookup (100×)', '', ' ms', ' μs avg', 'Point lookup (100 queries)', analysis_point)
    replace_row('Range scan (5%)', '', ' ms', ' rows/s', 'Range scan (top 5%)', analysis_range)
    replace_row('INNER JOIN (100K × 1K)', '', ' ms', ' rows/s', 'INNER JOIN (100000 x 1K)', analysis_join)

    with open('DesignDoc.md', 'w') as f:
        f.write(content)

    print("Successfully updated DesignDoc.md with latest benchmark results.")

if __name__ == '__main__':
    main()
