# ============================================================
#  monitor_box - 监控盒子 (Quectel M2 / RK3576) 交叉编译
# ------------------------------------------------------------
#  实际编译由 CMake 完成, 这个 Makefile 只负责“一次配好、顺手装上”:
#    make                配置 + 编译 (产物 build/mb-infer / mb-capture / mb-selftest / mb-ruletest)
#    make ruletest       宿主机原生编译并运行规则/跟踪回归 (不需要交叉工具链和板子)
#    make configtest     验证旧版持久配置迁移 (不需要板子)
#    make apptest        运行桌面 APP 单元测试 (不需要板子)
#    make test           configtest + apptest + ruletest + osdtest
#    make info           打印工具链 / sysroot / 库路径
#    make smoke          qemu-aarch64 里跑一遍做冒烟验证 (NPU 那段必然失败)
#    make install        放入 overlay/opt/monitor_box/, 随固件打包
#    make image          install + 重新打包 build/result/update.img
#    make clean          清理编译产物
#    make uninstall      从 overlay/ 撤掉已安装的文件
#
#  可用变量:
#    SDK_ROOT=<路径>      SDK 根目录 (默认按本 Makefile 位置推算)
#    PREFIX=<路径>        板上安装前缀 (默认 /opt/monitor_box)
#    BUILD_TYPE=<类型>    Debug / Release (默认 Release)
# ============================================================

APP         := mb-infer
CAPTURE     := mb-capture
SELFTEST    := mb-selftest
RULETEST    := mb-ruletest
APPS        := $(APP) $(CAPTURE) $(SELFTEST) $(RULETEST)
SDK_ROOT    ?= $(abspath $(CURDIR)/../..)
SYSROOT     ?= $(SDK_ROOT)/prebuilds/sysroot
BUILD_DIR   ?= build
PREFIX      ?= /opt/monitor_box
OVERLAY_DIR ?= $(SDK_ROOT)/overlay
BUILD_TYPE  ?= Release

CMAKE       := cmake
TC_FILE     := $(SDK_ROOT)/tools/cmake/aarch64-m2-debian-toolchain.cmake
PACK_SCRIPT := $(SDK_ROOT)/tools/build-kernel.sh
TARGET      := $(BUILD_DIR)/$(APP)

# Canonical model set. YOLOv8s is the default; YOLOv8n is an optional
# low-power detector. Keep these names aligned with config/monitor_box.json,
# install.sh and tools/build_on_board.sh.
DEFAULT_MODEL    := yolov8s-640-640.rknn
OPTIONAL_MODELS  := yolov8n-640-640.rknn fire_yolov8n.rknn rtmpose_t-256x192-rk3576.rknn
MODEL_FILES      := $(DEFAULT_MODEL) coco_80_labels_list.txt $(OPTIONAL_MODELS) fire_labels.txt
SMOKE_MODEL      := models/$(DEFAULT_MODEL)
SMOKE_LABELS     := models/coco_80_labels_list.txt
SMOKE_IMAGE      := $(SYSROOT)/usr/share/model/yolov5/model/bus.jpg

# CMake 自己会调交叉工具链; 这里只把 sysroot 交代清楚.
# 直接 `make` 也能跑 (不依赖 source build.sh)。
CMAKE_ARGS := -S . -B $(BUILD_DIR) \
              -DCMAKE_TOOLCHAIN_FILE=$(TC_FILE) \
              -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
              -DCMAKE_SYSROOT=$(SYSROOT)

ifeq ($(origin QPI_SDK_TOPDIR),undefined)
  # 没 source build.sh 时, CMAKE_TOOLCHAIN_FILE 里的路径仍然有效 (它自己算 SDK 根)。
  export QPI_SDK_TOPDIR := $(SDK_ROOT)
endif

# ------------------------------------------------------------
# 规则
# ------------------------------------------------------------
all: $(TARGET)

$(TARGET): $(shell find src examples third_party CMakeLists.txt -type f 2>/dev/null)
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) $(CMAKE_ARGS)
	$(CMAKE) --build $(BUILD_DIR)
	@echo "[OK] 产物: $(TARGET) ($$(stat -c%s $(TARGET)) 字节)"

# 冒烟: 分两段
#   1) --preprocess-only: 解码 + letterbox + 存模型输入, 宿主机也能全部通过
#   2) 完整推理: 宿主机没有 /dev/rknpu, 预期止步于 rknn_init, 这里只检查它确实走到了
#      模型加载这一步 (说明命令行/读图都正常), 不算失败。
smoke: $(TARGET)
	@if command -v qemu-aarch64-static >/dev/null 2>&1; then \
		set -e; \
		echo "--- 1/2 预处理链路 (不碰 NPU) ---"; \
		QEMU_LD_PREFIX=$(SYSROOT) qemu-aarch64-static $(TARGET) \
			--preprocess-only --input-size 640x640 \
			--image $(SMOKE_IMAGE) --dump-pre /tmp/mb-infer-smoke-input.jpg; \
		echo "--- 2/2 完整推理 (宿主机无 NPU, 止步于 rknn_init 属预期) ---"; \
		out=$$(QEMU_LD_PREFIX=$(SYSROOT) qemu-aarch64-static $(TARGET) \
			--model $(SMOKE_MODEL) --image $(SMOKE_IMAGE) \
			--labels $(SMOKE_LABELS) --output /tmp/mb-infer-smoke.jpg 2>&1 || true); \
		echo "$$out"; \
		echo "$$out" | grep -q "模型加载失败\|检出 " || { echo "[FAIL] 没有进入模型加载/推理阶段"; exit 1; }; \
		if echo "$$out" | grep -q "检出 "; then \
			echo "[OK] 冒烟通过 (NPU 可用, 完成一次完整推理)"; \
		else \
			echo "[OK] 冒烟通过 (预处理链路可用; 本机无 NPU, 推理部分需上板验证)"; \
		fi; \
	else \
		echo "[SKIP] 未安装 qemu-aarch64-static, 跳过冒烟"; \
	fi

# 放进 overlay/ 对应目录 (目录结构 == rootfs 路径), 由打包脚本写入镜像
install: $(TARGET)
	@test -d "$(SYSROOT)/usr/include" || { echo "[ERROR] sysroot 不存在: $(SYSROOT)"; exit 1; }
	@test -r models/$(DEFAULT_MODEL) || { echo "[ERROR] 缺少 models/$(DEFAULT_MODEL)"; exit 1; }
	@test -r models/coco_80_labels_list.txt || { echo "[ERROR] 缺少 models/coco_80_labels_list.txt"; exit 1; }
	install -D -m 0755 $(TARGET) $(OVERLAY_DIR)$(PREFIX)/bin/$(APP)
	install -D -m 0755 $(BUILD_DIR)/$(CAPTURE) $(OVERLAY_DIR)$(PREFIX)/bin/$(CAPTURE)
	install -D -m 0755 $(BUILD_DIR)/$(SELFTEST) $(OVERLAY_DIR)$(PREFIX)/bin/$(SELFTEST)
	install -D -m 0755 $(BUILD_DIR)/$(RULETEST) $(OVERLAY_DIR)$(PREFIX)/bin/$(RULETEST)
	install -D -m 0644 config/monitor_box.json $(OVERLAY_DIR)$(PREFIX)/etc/monitor_box/monitor_box.json
	@for f in $(MODEL_FILES); do \
		if [ -f "models/$$f" ]; then \
			install -D -m 0644 "models/$$f" "$(OVERLAY_DIR)$(PREFIX)/models/$$f"; \
		fi; \
	done
	@echo "[OK] 已安装到 $(OVERLAY_DIR)$(PREFIX)/"
	@echo "     打包进镜像: make image   (或 $(PACK_SCRIPT) pack)"

image: install
	@test -f "$(SDK_ROOT)/kernel/Module.symvers" || \
		{ echo "[ERROR] 缺少 kernel/Module.symvers, 请先编译内核"; exit 1; }
	"$(PACK_SCRIPT)" pack

uninstall:
	rm -f "$(OVERLAY_DIR)$(PREFIX)/bin/$(APP)" "$(OVERLAY_DIR)$(PREFIX)/bin/$(CAPTURE)" "$(OVERLAY_DIR)$(PREFIX)/bin/$(SELFTEST)" "$(OVERLAY_DIR)$(PREFIX)/bin/$(RULETEST)"
	rm -f "$(OVERLAY_DIR)$(PREFIX)/etc/monitor_box/monitor_box.json"
	@for f in $(MODEL_FILES); do rm -f "$(OVERLAY_DIR)$(PREFIX)/models/$$f"; done
	@rmdir "$(OVERLAY_DIR)$(PREFIX)/bin" "$(OVERLAY_DIR)$(PREFIX)/etc/monitor_box" "$(OVERLAY_DIR)$(PREFIX)/etc" "$(OVERLAY_DIR)$(PREFIX)/models" 2>/dev/null || true
	@rmdir "$(OVERLAY_DIR)$(PREFIX)" 2>/dev/null || true

info:
	@echo "SDK_ROOT  : $(SDK_ROOT)"
	@echo "SYSROOT   : $(SYSROOT)"
	@echo "CMake 工具链: $(TC_FILE)"
	@echo "BUILD_TYPE: $(BUILD_TYPE)"
	@echo "产物      : $(TARGET)"
	@echo "安装路径  : $(OVERLAY_DIR)$(PREFIX)/bin/$(APP)"
	@echo "默认配置  : $(OVERLAY_DIR)$(PREFIX)/etc/monitor_box/monitor_box.json"
	@if [ -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		grep -E "^(CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|RKNNRT_LIB|RGA_LIB|JPEG_LIB):" \
			$(BUILD_DIR)/CMakeCache.txt; \
	else \
		echo "(还没配置过, 先跑一次 make)"; \
	fi

clean:
	rm -rf $(BUILD_DIR)

# 规则/跟踪只依赖标准库, 宿主机直接编译跑, 改阈值逻辑后先过这一关再上板
RULETEST_SRCS := examples/ruletest.cpp src/rules/rule_engine.cpp src/rules/rule_bindings.cpp \
                 src/rules/fire_detector.cpp \
                 src/rules/activity_map.cpp src/rules/count_store.cpp src/video/view_guard.cpp \
                 src/common/geometry.cpp src/common/json.cpp src/track/tracker.cpp \
                 src/locate/ground_calib.cpp src/locate/ground_locator.cpp src/geo/geo_locator.cpp \
                 src/event/dwell_guard.cpp src/event/speaker.cpp src/event/zone_alarm.cpp
configtest:
	python3 -m unittest tools.test_migrate_config -v

apptest:
	cd app && python3 -m unittest discover -s . -p 'test_*.py' -v

test: configtest apptest ruletest osdtest

ruletest:
	@mkdir -p $(BUILD_DIR)/host
	g++ -std=c++17 -O1 -g -Wall -Wextra -Isrc $(RULETEST_SRCS) -o $(BUILD_DIR)/host/$(RULETEST)
	$(BUILD_DIR)/host/$(RULETEST)

osdtest:
	@mkdir -p $(BUILD_DIR)/host
	$(CC) -std=c11 -O1 -include math.h -Ithird_party/zoo -c third_party/zoo/image_drawing.c -o $(BUILD_DIR)/host/osd_drawing.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Isrc -Ithird_party/zoo examples/osdtest.cpp src/video/region_overlay.cpp src/video/bev_osd.cpp src/common/json.cpp src/common/geometry.cpp $(BUILD_DIR)/host/osd_drawing.o -lm -o $(BUILD_DIR)/host/mb-osdtest
	$(BUILD_DIR)/host/mb-osdtest

# Access the running Web UI through ADB (works even when the board has no LAN IP).
web-adb:
	bash tools/adb_web.sh --bind "$$(ip -4 -br addr show scope global | awk 'NR == 1 { split($$3, a, "/"); print a[1] }')"

.PHONY: all smoke configtest apptest test ruletest osdtest install image uninstall info clean web-adb
