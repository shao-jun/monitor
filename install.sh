#!/usr/bin/env bash
set -euo pipefail

# 构建并安装 DDE 任务栏插件「应用使用时间」到 /usr/lib/dde-dock/plugins/
# 用法: sudo ./install.sh

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$HERE/build"
PLUGIN_DIR="/usr/lib/dde-dock/plugins"
TARGET="libapp-usage-monitor.so"

if [[ "${EUID}" -ne 0 ]]; then
    echo "请使用 sudo 运行: sudo ./install.sh"
    exit 1
fi

echo "==> 配置并构建"
cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> 安装插件"
install -m 755 "$BUILD_DIR/out/$TARGET" "$PLUGIN_DIR/$TARGET"

echo "==> 重启任务栏 (dde-dock)"
if pidof dde-dock >/dev/null 2>&1; then
    pkill -x dde-dock || true
fi
sleep 1
nohup dde-dock >/dev/null 2>&1 &

echo "完成。插件已安装并重新加载任务栏。"
