# SmartLED ESP32 Gateway

本项目使用 ESP32-WROOM-32 通过 BLE 控制兼容的 Xiaomi MIoT BLE 灯具，并通过 Wi-Fi 和 MQTT 暴露本地控制接口。它不依赖 Home Assistant；任何 MQTT 客户端都可以发送命令并读取状态。

## 工作方式

```text
MQTT client / knob ── Wi-Fi ── MQTT broker ── ESP32-WROOM-32 ── BLE ── light
```

ESP32 负责 BLE 连接、灯光属性读写和 MQTT 状态发布。旋钮等客户端只需要订阅状态主题、向命令主题发布文本命令。

## 当前能力

- 通过 MIoT BLE v2 登录并读取电源、亮度和色温。
- 设置电源、亮度和色温，并在写入后读取状态确认。
- 通过 MQTT 接收控制命令并发布灯具状态。
- Always Ready v3：空闲 BLE 回读改为 30 秒，控制命令仍立即回读；配套旋钮保持黑屏方向控灯。当前版本已编译、烧录并验证一次同值 V1 命令的真实 BLE 回读；配套旋钮基础开关手感和黑屏交互已获用户确认，12 小时续航留待后续实测。
- 断开 BLE 后重新扫描和连接；网络和设备恢复行为仍应结合目标硬件验证。

验证状态和协议依据见 [docs/PROJECT_MEMORY.md](docs/PROJECT_MEMORY.md) 与 [docs/RESEARCH.md](docs/RESEARCH.md)。编译成功、实机验证和待验证事项分开记录。

## 目录

| 路径 | 内容 |
| --- | --- |
| `main/` | ESP-IDF 应用、BLE 连接、MIoT 编解码、加密和 MQTT 网关 |
| `tools/` | BLE 只读探针、设备密钥本地注入工具及离线测试 |
| `docs/` | 首次配置、网关主题、协议研究、验证记录和路线图 |
| `sdkconfig.defaults` / `dependencies.lock` | 不含凭据的 ESP-IDF 默认配置与组件锁定版本 |

本地构建目录、`sdkconfig`、设备密钥头文件、密钥和抓包均不属于仓库内容。请勿提交编译出的固件镜像；目标设备密钥会编入镜像。

## 构建

安装 ESP-IDF 6.x 并设置 `IDF_PATH`，然后从仓库根目录运行：

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32
idf.py build
```

如需运行主机侧 BLE 工具，请按 [docs/FIRST_RUN.md](docs/FIRST_RUN.md) 安装 Python 依赖。构建固件不需要设备密钥；烧录前需要在本机生成被忽略的私有头文件。

## 本机配置与烧录

1. 按 [docs/X_KNOB_GATEWAY.md](docs/X_KNOB_GATEWAY.md) 配置 Wi-Fi、MQTT broker 和 topic 前缀。所有网络凭据只保存在本机 `sdkconfig`。
2. 使用 `tools/provision_firmware_key.py` 从仓库外权限受限文件生成 `main/gatt_ltmk_private.h`。
3. 构建完成后，先核对目标板和串口，再运行 `idf.py -p <serial-port> flash`。

请自行确认烧录会覆盖目标板当前固件。不要把真实凭据、设备标识、密钥、串口原始日志或包含密钥的镜像上传到 issue、聊天或仓库。

## 协议边界

固件只应对精确识别且协议已验证的目标型号进行认证和控制。通用 Xiaomi 广播或其他型号的控制帧不能作为本型号支持的证据。阅读 BLE 协议实现前，请先看 [docs/PROJECT_MEMORY.md](docs/PROJECT_MEMORY.md) 和 [docs/ROADMAP.md](docs/ROADMAP.md)。
