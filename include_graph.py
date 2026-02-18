#!/usr/bin/env python3
import os, re, sys
from collections import defaultdict, deque

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')

def iter_sources(root, exts=(".h",".hpp",".hh",".c",".cc",".cpp",".cxx")):
    for dirpath, dirnames, filenames in os.walk(root):
        # 过滤掉常见无关目录
        skip = {".git", "build", "cmake-build-debug", "cmake-build-release", "out", "bin", ".cache"}
        dirnames[:] = [d for d in dirnames if d not in skip]
        for fn in filenames:
            if fn.endswith(exts):
                yield os.path.join(dirpath, fn)

def read_includes(path):
    inc = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = INCLUDE_RE.match(line)
                if m:
                    inc.append(m.group(1))
    except Exception:
        pass
    return inc

def build_index(root):
    # 把 "RtspServer.h" -> /abs/path/.../RtspServer.h
    index = defaultdict(list)
    for p in iter_sources(root):
        base = os.path.basename(p)
        index[base].append(os.path.abspath(p))
    return index

def resolve(include, cur_dir, index):
    # 优先相对路径
    rel = os.path.abspath(os.path.join(cur_dir, include))
    if os.path.exists(rel):
        return rel
    # 再按 basename 匹配（不完美，但够找循环）
    base = os.path.basename(include)
    lst = index.get(base)
    if lst:
        # 多个同名时先选第一个
        return lst[0]
    return None

def find_cycles(graph):
    # DFS 找环
    WHITE, GRAY, BLACK = 0, 1, 2
    color = defaultdict(int)
    parent = {}
    cycles = []

    def dfs(u):
        color[u] = GRAY
        for v in graph[u]:
            if color[v] == WHITE:
                parent[v] = u
                dfs(v)
            elif color[v] == GRAY:
                # found back-edge u->v, reconstruct cycle
                cyc = [v]
                x = u
                while x != v and x in parent:
                    cyc.append(x)
                    x = parent[x]
                cyc.append(v)
                cyc.reverse()
                cycles.append(cyc)
        color[u] = BLACK

    for n in list(graph.keys()):
        if color[n] == WHITE:
            parent[n] = None
            dfs(n)
    return cycles

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    root = os.path.abspath(root)
    index = build_index(root)

    graph = defaultdict(set)
    nodes = set()

    for src in iter_sources(root):
        nodes.add(os.path.abspath(src))
        cur_dir = os.path.dirname(os.path.abspath(src))
        for inc in read_includes(src):
            dep = resolve(inc, cur_dir, index)
            if dep:
                graph[os.path.abspath(src)].add(dep)
                nodes.add(dep)

    # 输出最大出度文件（include 最多的）
    outdeg = sorted(((n, len(graph[n])) for n in graph), key=lambda x: x[1], reverse=True)
    print("Top include-heavy files:")
    for n, d in outdeg[:20]:
        print(f"{d:4d}  {os.path.relpath(n, root)}")

    # 找环
    cycles = find_cycles(graph)
    print("\nInclude cycles found:", len(cycles))
    for i, cyc in enumerate(cycles[:30], 1):
        print(f"\nCycle #{i}:")
        for p in cyc:
            print("  ->", os.path.relpath(p, root))

    # 生成 dot 图（可选）
    dot_path = os.path.join(root, "include_graph.dot")
    with open(dot_path, "w", encoding="utf-8") as f:
        f.write("digraph includes {\n")
        f.write('  rankdir="LR";\n')
        for u in graph:
            for v in graph[u]:
                f.write(f'  "{os.path.relpath(u, root)}" -> "{os.path.relpath(v, root)}";\n')
        f.write("}\n")
    print(f"\nWrote: {dot_path}")
    print("Render with: dot -Tpng include_graph.dot -o include_graph.png")

if __name__ == "__main__":
    main()
