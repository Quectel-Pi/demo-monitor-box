#!/usr/bin/env bash
# ============================================================================
#  监控盒子 - 开机自启动安装脚本 (Quectel Pi M2 / RK3576)
# ----------------------------------------------------------------------------
#  用法: ./setup_autostart.sh [install|uninstall|status]
#
#  这里用**系统级** systemd 服务, 而不是参考项目 demo-gesture-remote-control
#  用的 systemd --user: 那个项目要开桌面窗口, 必须挂在 graphical-session 上;
#  本项目是无人值守的网络设备, 开机就要出 /stream 和告警, 不该等用户登录。
#  (systemd --user 在没有 linger 的情况下要等登录才拉起, headless 会踩坑。)
# ============================================================================
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/opt/monitor_box}"
SERVICE_NAME="monitor-box"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
CONFIG="${PREFIX}/etc/monitor_box/monitor_box.json"
LOG_DIR="${PREFIX}/log"
# 主程序是 mb-capture (摄像头 -> 检测 -> 跟踪 -> 规则 -> OSD)。
# 想跑别的二进制可以用 MB_APP 覆盖, 例如 MB_APP=mb-infer 只跑离线工具。
APP_BIN="${PREFIX}/bin/${MB_APP:-mb-capture}"
SYSCTL_FILE="$APP_DIR/app/99-monitor-box-video.conf"

generate_service() {
    echo "==> 生成服务文件..."
    # 等哪个节点就绪要看配置里的 video.device (默认是 /dev/video73, 不是 /dev/video0)
    local video_dev="/dev/video0"
    if [ -r "$CONFIG" ]; then
        local d
        d=$(sed -n 's/.*"device"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG" | head -1)
        [ -n "$d" ] && video_dev="$d"
    fi
    echo "  等待的采集节点: $video_dev"
    cat > /tmp/${SERVICE_NAME}.service << EOF
[Unit]
Description=Monitor Box - 监控盒子 (AI 视频分析)
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
Type=simple
# 系统自带的相机服务会占住 /dev/video*, 先停掉再起
ExecStartPre=-/usr/bin/systemctl stop cam-server.service cam2file.service
# 等摄像头节点就绪 (最多 15 秒; 没插相机也照常启动, 由程序自己重试)
ExecStartPre=/bin/bash -c 'for i in \$(seq 1 15); do [ -e ${video_dev} ] && exit 0; sleep 1; done; exit 0'
ExecStart=${APP_BIN} --config ${CONFIG}
WorkingDirectory=${PREFIX}
Restart=always
RestartSec=2
StandardOutput=append:${LOG_DIR}/service.log
StandardError=append:${LOG_DIR}/service_error.log

[Install]
WantedBy=multi-user.target
EOF
    sudo install -D -m 0644 /tmp/${SERVICE_NAME}.service "$SERVICE_FILE"
    rm -f /tmp/${SERVICE_NAME}.service
    sudo install -D -m 0644 "$SYSCTL_FILE" \
        /etc/sysctl.d/99-monitor-box-video.conf
    sudo sysctl --system >/dev/null
    sudo mkdir -p "$LOG_DIR"
    echo "  [OK] 已生成: $SERVICE_FILE"
}

install_service() {
    echo "==> 安装服务..."
    sudo systemctl daemon-reload
    sudo systemctl enable "$SERVICE_NAME"
    echo "  [OK] 已启用开机自启"
}

start_service() {
    echo "==> 启动服务..."
    if ! sudo systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
        sudo systemctl start "$SERVICE_NAME"
    else
        sudo systemctl restart "$SERVICE_NAME"
    fi
    sleep 2
    if sudo systemctl is-active --quiet "$SERVICE_NAME"; then
        echo "  [OK] 服务已启动"
    else
        echo "  [ERROR] 服务没有起来, 状态:"
        sudo systemctl status "$SERVICE_NAME" --no-pager -l || true
        exit 1
    fi
}

uninstall_service() {
    echo "==> 卸载服务..."
    sudo systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    sudo systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    sudo rm -f "$SERVICE_FILE"
    sudo systemctl daemon-reload
    sudo systemctl reset-failed "$SERVICE_NAME" 2>/dev/null || true
    echo "  [OK] 已卸载"
}

show_status() {
    echo "==> 服务状态 =="
    echo "  服务名  : $SERVICE_NAME"
    echo "  服务文件: $SERVICE_FILE"
    echo "  可执行  : $APP_BIN"
    echo "  配置    : $CONFIG"
    echo ""
    if [ -f "$SERVICE_FILE" ]; then
        echo "  已安装  : 是"
        sudo systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null \
            && echo "  开机自启: 是" || echo "  开机自启: 否"
        sudo systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null \
            && echo "  运行状态: 运行中" || echo "  运行状态: 已停止"
        echo ""
        sudo systemctl status "$SERVICE_NAME" --no-pager -l 2>/dev/null || true
    else
        echo "  已安装  : 否"
    fi
}

print_usage() {
    echo "用法: $0 [install|uninstall|status]"
    echo "  install   - 安装并启用开机自启动 (默认)"
    echo "  uninstall - 停止并移除服务"
    echo "  status    - 查看服务状态"
    echo ""
    echo "手动控制:"
    echo "  sudo systemctl start   $SERVICE_NAME   # 启动"
    echo "  sudo systemctl stop    $SERVICE_NAME   # 停止"
    echo "  sudo systemctl restart $SERVICE_NAME   # 重启"
    echo "  journalctl -u $SERVICE_NAME -f         # 查看系统日志"
    echo "  tail -f ${LOG_DIR}/service.log         # 查看程序输出"
}

ACTION="${1:-install}"

case "$ACTION" in
    install)
        if [ ! -x "$APP_BIN" ] || [ ! -r "$CONFIG" ]; then
            echo "[ERROR] 缺少 $APP_BIN 或 $CONFIG; 请先执行 ./install.sh。"
            exit 1
        fi
        if [ ! -r "$SYSCTL_FILE" ]; then
            echo "[ERROR] 缺少 $SYSCTL_FILE; 请按 README 重新推送 app/99-monitor-box-video.conf。"
            exit 1
        fi
        generate_service
        install_service
        start_service
        echo ""
        echo "============================================"
        echo "  安装完成！开机后将自动启动监控盒子"
        echo "  手动启动: sudo systemctl start $SERVICE_NAME"
        echo "  查看日志: journalctl -u $SERVICE_NAME -f"
        echo "============================================"
        ;;
    uninstall) uninstall_service ;;
    status)    show_status ;;
    -h|--help|help) print_usage ;;
    *)
        echo "[ERROR] 未知操作: $ACTION"
        print_usage
        exit 1
        ;;
esac
