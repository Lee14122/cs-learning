#!/usr/bin/env bash
# 依次运行全部实验（04 会创建大量 fd，05 自带自测客户端）
cd "$(dirname "$0")"
for p in bin/01_epoll_basics bin/02_lt_vs_et bin/03_epoll_ctl bin/04_scale_epoll_vs_poll bin/05_mini_muduo; do
    echo
    echo "==================== $p ===================="
    timeout 60 "$p"
done
