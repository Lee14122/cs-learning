#!/usr/bin/env bash
# 编译全部 epoll 实验，产物在 bin/
set -e
cd "$(dirname "$0")"
mkdir -p bin

for f in src/0*.c; do
    name=$(basename "$f" .c)
    gcc -std=gnu11 -O2 -Wall -Wextra -o "bin/$name" "$f"
    echo "✔ 编译 $name"
done

g++ -std=c++17 -O2 -Wall -Wextra -pthread -o bin/05_mini_muduo src/05_mini_muduo.cpp
echo "✔ 编译 05_mini_muduo"

echo "完成。逐个运行 ./bin/0X_*，或 ./run_all.sh 看全部输出"
