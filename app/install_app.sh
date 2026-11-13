#!/usr/bin/env bash
# ============================================================================
#  监控盒子 APP - 部署到板子
# ----------------------------------------------------------------------------
#  APP 是纯 Python, 不需要编译, 所以这个脚本只做「推文件 + 装桌面入口/图标 + 查权限」。
#
#  用法 (在 SDK 宿主机上, 板子已 adb 连上):
#      ./app/install_app.sh                 # 推到 /opt/monitor_box/app
#      ./app/install_app.sh --local         # 在板子上直接跑 (复制到 /opt)
#      ADB="adb -s <serial>" ./app/install_app.sh
#
#  装完在 GNOME 应用列表里搜 "Monitor Box" / "监控盒子"。
#  板上没装桌面图标插件, 图标只在应用列表 (按 Super 输入名字)。
# ============================================================================
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/opt/monitor_box/app}"
DESKTOP_DIR="${DESKTOP_DIR:-/usr/share/applications}"
FILES="mb_app.py mb_i18n.py mb_ui.py mb_source.py mb_settings.py mb_client.py mb_heatmap.py mb_regions.py mb_region_editor.py mb_bev.py mb_ground.py monitor-box-app com.quectel.MonitorBox.desktop 99-monitor-box-video.conf"
ICON_THEME_DIR="${ICON_THEME_DIR:-/usr/share/icons/hicolor}"
ICON_NAME="com.quectel.MonitorBox"
DESKTOP_NAME="com.quectel.MonitorBox.desktop"
LEGACY_DESKTOP_NAME="mb-app.desktop"
ICON_SIZES="16x16 24x24 32x32 48x48 64x64 128x128 256x256 512x512"

LOCAL=0
[ "${1:-}" = "--local" ] && LOCAL=1

say() { echo "==> $*"; }

say "仅更新 Python 桌面 APP，不更新设备端 mb-capture（检测/画框/录像）"
say "后端更新请在 SDK 宿主机运行: bash $APP_DIR/../tools/update_backend_adb.sh"

# ---------------------------------------------------------------- 收尾自检
# 这条命令在板上执行 (本地安装时直接跑)。@PREFIX@ 稍后替换成实际路径 ——
# 这里用单引号 heredoc, 免得定义时就把 $(...) 展开成宿主机的输出。
POST_CHECK=$(cat <<'EOS'
echo "  已安装: $(ls -l '@PREFIX@/monitor-box-app' 2>/dev/null | cut -d' ' -f1,9-)"
echo "  桌面入口: $(ls -l '@DESKTOP_DIR@/@DESKTOP_NAME@' 2>/dev/null | cut -d' ' -f1,9-)"
echo "  应用图标: $(ls -l '@ICON_THEME_DIR@/512x512/apps/@ICON_NAME@.png' 2>/dev/null | cut -d' ' -f1,9-)"
python3 - '@PREFIX@' <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
try:
    import mb_bev, mb_ground, mb_heatmap, mb_i18n, mb_settings, mb_source
except Exception as e:
    print("  [ERROR] 模块导入失败:", e); sys.exit(1)
s = mb_settings.Settings()
print("  配置文件:", s.path or "<内置默认>", "(可写)" if s.writable else "(只读!)")
dev = mb_source.resolve_device(s)
print("  摄像头节点:", dev, "空闲" if mb_source.camera_free(dev) else "被占用 (mb-capture 在跑?)")
print("  Web 服务在:", mb_source.port_open())
print("  自动选源:", mb_source.plan(s))
try:
    import gi
    gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")
    from gi.repository import Gtk, Adw
    print("  GTK %d.%d / Adw %d.%d" % (Gtk.MAJOR_VERSION, Gtk.MINOR_VERSION,
                                       Adw.MAJOR_VERSION, Adw.MINOR_VERSION))
except Exception as e:
    print("  [ERROR] GTK4/Adw 不可用:", e); sys.exit(1)
PY
# APP 由桌面用户运行; root 自检不能证明它有原子替换配置的权限。
for c in /etc/monitor_box/monitor_box.json /opt/monitor_box/etc/monitor_box/monitor_box.json; do
    [ -f "$c" ] || continue
    echo "  设置保存请先启动 mb-capture, APP 通过 /config 写入; 不要放宽配置目录权限。"
    break
done
if [ ! -f /etc/monitor_box/monitor_box.json ] && [ ! -f /opt/monitor_box/etc/monitor_box/monitor_box.json ]; then
    echo "  [WARN] 未安装后端配置。请先部署 mb-capture 和 config/monitor_box.json, 否则设置无法保存。"
fi
EOS
)
POST_CHECK=$(printf '%s\n' "$POST_CHECK" | sed "s|@PREFIX@|$PREFIX|g; s|@DESKTOP_DIR@|$DESKTOP_DIR|g; s|@ICON_THEME_DIR@|$ICON_THEME_DIR|g; s|@ICON_NAME@|$ICON_NAME|g; s|@DESKTOP_NAME@|$DESKTOP_NAME|g")

# ---------------------------------------------------------------- 检查
say "检查文件"
for f in $FILES; do
    [ -f "$APP_DIR/$f" ] || { echo "  缺少 $f"; exit 1; }
done
sh -n "$APP_DIR/monitor-box-app" || { echo "  monitor-box-app 语法错误"; exit 1; }
for size in $ICON_SIZES; do
    [ -f "$APP_DIR/icons/hicolor/$size/apps/$ICON_NAME.png" ] || {
        echo "  缺少应用图标: icons/hicolor/$size/apps/$ICON_NAME.png"; exit 1; }
done

# 桌面入口的 Exec 必须是绝对路径 (图标点开时 PATH 不含 /opt/monitor_box/app)
DESKTOP_TMP="$(mktemp)"
trap 'rm -f "$DESKTOP_TMP"' EXIT
sed "s|^Exec=monitor-box-app|Exec=$PREFIX/monitor-box-app|" \
    "$APP_DIR/$DESKTOP_NAME" >"$DESKTOP_TMP"
grep -q "^Exec=$PREFIX/monitor-box-app" "$DESKTOP_TMP" || {
    echo "  生成 .desktop 失败: Exec 没替换成 $PREFIX"; exit 1; }

# ---------------------------------------------------------------- 本地安装
if [ "$LOCAL" -eq 1 ]; then
    say "安装到 $PREFIX"
    sudo install -d -m 0755 "$PREFIX"
        sudo install -m 0644 "$APP_DIR/mb_app.py" "$APP_DIR/mb_i18n.py" "$APP_DIR/mb_ui.py" "$APP_DIR/mb_source.py" \
        "$APP_DIR/mb_settings.py" "$APP_DIR/mb_client.py" "$APP_DIR/mb_heatmap.py" "$APP_DIR/mb_regions.py" \
        "$APP_DIR/mb_region_editor.py" "$APP_DIR/mb_bev.py" "$APP_DIR/mb_ground.py" "$PREFIX"/
    sudo install -m 0755 "$APP_DIR/monitor-box-app" "$PREFIX"/
    say "安装应用图标"
    for size in $ICON_SIZES; do
        sudo install -D -m 0644 "$APP_DIR/icons/hicolor/$size/apps/$ICON_NAME.png" \
            "$ICON_THEME_DIR/$size/apps/$ICON_NAME.png"
    done
    sudo gtk-update-icon-cache -f -t "$ICON_THEME_DIR" >/dev/null 2>&1 || true
    sudo install -m 0644 "$DESKTOP_TMP" "$DESKTOP_DIR/$DESKTOP_NAME"
    sudo unlink "$DESKTOP_DIR/$LEGACY_DESKTOP_NAME" 2>/dev/null || true
    sudo update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
    sudo install -m 0644 "$APP_DIR/99-monitor-box-video.conf" \
        /etc/sysctl.d/99-monitor-box-video.conf
    sudo sysctl --system >/dev/null

    say "自检"
    sh -c "$POST_CHECK"
    cat <<EOF

部署完成。
  应用    : $PREFIX/monitor-box-app
  入口    : $DESKTOP_DIR/$DESKTOP_NAME
  图标    : $ICON_THEME_DIR/512x512/apps/$ICON_NAME.png
  手动跑  : $PREFIX/monitor-box-app --source uvc
  桌面里  : 按 Super 输入 "监控盒子" / "Monitor Box"

EOF
    exit 0
fi

# ---------------------------------------------------------------- adb 安装
ADB="${ADB:-adb}"
"$ADB" get-state >/dev/null 2>&1 || { echo "板子没连上 (adb devices 为空)"; exit 1; }

# adb shell 通常是 root; 不是就退到 sudo -n
as_root() {
    if "$ADB" shell "$*" >/dev/null 2>&1; then return 0; fi
    "$ADB" shell "sudo -n $*"
}

say "推送文件"
for f in $FILES; do
    "$ADB" push "$APP_DIR/$f" "/tmp/$f" >/dev/null
done
# 已替换好 Exec 的 desktop 文件 (不在板上跑 sed: adb shell 会多解释一层引号)
"$ADB" push "$DESKTOP_TMP" "/tmp/$DESKTOP_NAME" >/dev/null
for size in $ICON_SIZES; do
    "$ADB" push "$APP_DIR/icons/hicolor/$size/apps/$ICON_NAME.png" \
        "/tmp/monitor-box-$size.png" >/dev/null
done

say "安装到 $PREFIX"
as_root "mkdir -p $PREFIX"
as_root "install -m 0644 /tmp/mb_app.py /tmp/mb_i18n.py /tmp/mb_ui.py /tmp/mb_source.py /tmp/mb_settings.py /tmp/mb_client.py /tmp/mb_heatmap.py /tmp/mb_regions.py /tmp/mb_region_editor.py /tmp/mb_bev.py /tmp/mb_ground.py $PREFIX/"
as_root "install -m 0755 /tmp/monitor-box-app $PREFIX/monitor-box-app"

say "安装应用图标"
for size in $ICON_SIZES; do
    as_root "install -D -m 0644 /tmp/monitor-box-$size.png $ICON_THEME_DIR/$size/apps/$ICON_NAME.png"
done
as_root "gtk-update-icon-cache -f -t $ICON_THEME_DIR" || true
say "安装桌面入口"
as_root "install -m 0644 /tmp/$DESKTOP_NAME $DESKTOP_DIR/$DESKTOP_NAME"
as_root "test ! -e $DESKTOP_DIR/$LEGACY_DESKTOP_NAME || unlink $DESKTOP_DIR/$LEGACY_DESKTOP_NAME" || true
as_root "update-desktop-database $DESKTOP_DIR" || true
as_root "install -m 0644 /tmp/99-monitor-box-video.conf /etc/sysctl.d/99-monitor-box-video.conf"
as_root "sysctl --system >/dev/null"

say "自检"
"$ADB" shell "$POST_CHECK"

cat <<EOF

部署完成。
  应用    : $PREFIX/monitor-box-app
  入口    : $DESKTOP_DIR/$DESKTOP_NAME
  图标    : $ICON_THEME_DIR/512x512/apps/$ICON_NAME.png
  手动跑  : $PREFIX/monitor-box-app --source uvc
  桌面里  : 按 Super 输入 "监控盒子" / "Monitor Box"

EOF
