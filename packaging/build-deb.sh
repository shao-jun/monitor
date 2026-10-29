#!/usr/bin/env bash
# 打 deb 安装包：sudo dpkg -i 即可安装的 dde-dock 插件包
# 用法：./packaging/build-deb.sh [版本号] [架构]
# 产物：dist/app-usage-monitor_<版本>_<架构>.deb
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-1.3.2}"
ARCH="${2:-$(dpkg --print-architecture)}"
PKG=app-usage-monitor
BUILD_DIR="$HERE/build-deb"
STAGE="$BUILD_DIR/stage"
OUT_DIR="$HERE/dist"
SO="$BUILD_DIR/out/libapp-usage-monitor.so"

echo "==> 编译 ($ARCH, Release)"
rm -rf "$BUILD_DIR"
cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null
[[ -f "$SO" ]] || { echo "编译产物缺失: $SO"; exit 1; }

echo "==> 组装文件树"
rm -rf "$STAGE"
install -D -m 755 "$SO" "$STAGE/usr/lib/dde-dock/plugins/libapp-usage-monitor.so"
install -D -m 755 "$HERE/packaging/app-usage-monitor-rest" "$STAGE/usr/bin/app-usage-monitor-rest"
install -D -m 755 "$BUILD_DIR/out/app-usage-monitor-export" "$STAGE/usr/bin/app-usage-monitor-export"
install -D -m 644 "$HERE/README.md" "$STAGE/usr/share/doc/$PKG/README.md"
install -D -m 644 "$HERE/LICENSE" "$STAGE/usr/share/doc/$PKG/copyright"
mkdir -p "$STAGE/usr/share/doc/$PKG"
cat > "$STAGE/usr/share/doc/$PKG/changelog.Debian" <<EOF
$PKG ($VERSION) unstable; urgency=medium

  * 应用使用时间统计插件：按应用/按小时记录操作与查看时长、锁屏时间
  * 任务栏弹窗：日视图（每小时用时堆叠图）、月视图（每日用时柱状图 + 应用排行）
  * 连续工作 30 分钟弹气泡提醒休息，锁屏满 3 分钟中断连续计时

 -- wangshaojun <wangshaojun@uniontech.com>  $(date -R)
EOF
gzip -9n "$STAGE/usr/share/doc/$PKG/changelog.Debian"

echo "==> 生成依赖"
TMPCTRL="$BUILD_DIR/ctrl-dep"
mkdir -p "$TMPCTRL/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: %s\n' "$PKG" "$PKG" "$ARCH" > "$TMPCTRL/debian/control"
SHDEPS="$(cd "$TMPCTRL" && dpkg-shlibdeps -O "$STAGE/usr/lib/dde-dock/plugins/libapp-usage-monitor.so" 2>/dev/null \
          | sed -n 's/^shlibs:Depends=//p')"
[[ -n "$SHDEPS" ]] || SHDEPS="libc6, libstdc++6, libqt5core5a, libqt5gui5, libqt5widgets5, libqt5sql5, libqt5dbus5, libx11-6"
DEPENDS="$SHDEPS, libqt5sql5-sqlite, xprintidle, dde-dock"

INSTALLED_SIZE="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: wangshaojun <wangshaojun@uniontech.com>
Installed-Size: $INSTALLED_SIZE
Depends: $DEPENDS
Recommends: tesseract-ocr, tesseract-ocr-chi-sim
Section: utils
Priority: optional
Homepage: https://github.com/shao-jun/monitor
Description: 深度/统信任务栏插件：应用使用时间统计
 在任务栏托盘常驻，按应用和小时统计使用时长：有键鼠输入算"操作"，
 窗口有焦点但无输入算"查看"，会话锁屏单独记"锁屏"。
 .
 点击托盘图标弹出面板：日视图显示当天每小时用时堆叠图（操作/查看/锁屏）
 与应用排行；月视图显示当月每日用时柱状图与应用总排行。
 .
 连续工作满 30 分钟会弹一条桌面气泡提醒休息；锁屏满 3 分钟视为中断。
 只支持 X11 会话，数据仅存本机 ~/.local/share/app-usage-monitor/usage.db。
EOF

# 安装/卸载脚本不碰 dde-dock：重启任务栏会闪一下，交给用户自己决定时机
cat > "$STAGE/DEBIAN/postinst" <<'PKGEOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    echo "已安装 app-usage-monitor。"
    echo "插件由 dde-dock 启动时加载，当前这轮任务栏看不到它，需要重启一次任务栏："
    echo "    kill \$(pgrep -x dde-dock)     # startdde 会自动拉起"
    echo "或者注销重新登录。"
fi
exit 0
PKGEOF

cat > "$STAGE/DEBIAN/postrm" <<'PKGEOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    echo "已移除 app-usage-monitor，重启任务栏后图标才会消失："
    echo "    kill \$(pgrep -x dde-dock)"
fi
exit 0
PKGEOF

chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

echo "==> 打包"
mkdir -p "$OUT_DIR"
DEB="$OUT_DIR/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$DEB" >/dev/null

echo
echo "完成：$DEB"
ls -lh "$DEB" | awk '{print "大小：", $5}'
echo "依赖：$DEPENDS"
( cd "$OUT_DIR" && sha256sum "$(basename "$DEB")" )
