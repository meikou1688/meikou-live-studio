#!/usr/bin/env python3
"""更新歷程（CHANGELOG.md）小工具。

  stamp FILE          把「## 下一版」改成這次編譯的版號與日期（讀 MEIKOU_VERSION 環境變數）
  add FILE TEXT       在「## 下一版」段落加一行；沒有這個段落就建立在最前面
  obs FILE TAG        把開頭「核心是 OBS 官方穩定版 x.y.z」改成新的版本
"""
import datetime
import os
import re
import sys

NEXT = "## 下一版"


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def stamp(path):
    version = os.environ.get("MEIKOU_VERSION", "").strip()
    if not version:
        print("MEIKOU_VERSION not set, nothing to stamp")
        return
    text = read(path)
    taipei = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
    heading = f"## {version}（{taipei:%Y-%m-%d}）"
    new = re.sub(r"^## 下一版[^\n]*$", heading, text, count=1, flags=re.M)
    if new != text:
        write(path, new)
        print(f"stamped: {heading}")
    else:
        print("no '## 下一版' section")


def add(path, line):
    text = read(path)
    bullet = line if line.startswith("- ") else "- " + line
    if re.search(r"^## 下一版", text, flags=re.M):
        # 加在「## 下一版」段落的最後一行之後
        m = re.search(r"^## 下一版[^\n]*\n(.*?)(?=^## |\Z)", text, flags=re.M | re.S)
        body = m.group(1).rstrip("\n")
        new_body = (body + "\n" if body else "") + bullet + "\n\n"
        text = text[: m.start(1)] + new_body + text[m.end(1):].lstrip("\n")
    else:
        m = re.search(r"^## ", text, flags=re.M)
        block = f"{NEXT}\n{bullet}\n\n"
        text = text[: m.start()] + block + text[m.start():] if m else text.rstrip("\n") + "\n\n" + block
    write(path, text)


def obs(path, tag):
    text = read(path)
    write(path, re.sub(r"核心是 OBS 官方穩定版 [0-9.]+", f"核心是 OBS 官方穩定版 {tag}", text, count=1))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "stamp":
        stamp(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "obs":
        obs(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == "add":
        add(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
