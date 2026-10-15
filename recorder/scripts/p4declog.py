"""p4watch.py / p4send.py が残すログを UTF-8 のテキストに戻す。

使い道: ログの日本語が b'...\xe3\x81\x82' のようにエスケープされて
grep しにくいので、readable なファイルへ変換してから読む。

使い方:
  python p4declog.py <入力ログ> <出力ログ>
"""
import ast
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: p4declog.py <in> <out>")
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    kept = 0
    with open(src, encoding="utf-8", errors="replace") as fin, \
            open(dst, "w", encoding="utf-8") as fout:
        for line in fin:
            m = re.search(r"b'.*'\s*$", line.rstrip("\n"))
            if m:
                try:
                    fout.write(ast.literal_eval(m.group(0)).decode("utf-8", "replace"))
                    kept += 1
                    continue
                except (ValueError, SyntaxError):
                    pass
            fout.write(line)
    print(f"decoded {kept} chunks -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
