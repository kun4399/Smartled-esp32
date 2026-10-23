# 可参考项目与适用范围

检索日期：2026-10-23。以下是项目作者或厂商的公开资料，不表示已验证适配本灯带。

| 资料 | 借鉴点 | 与本项目的边界 |
| --- | --- | --- |
| [小米 BLE Mesh 基础知识](https://iot.mi.com/v2/new/doc/embedded-dev/ble-mesh/mesh_basic) | 官方描述 BLE Mesh 与 GATT 直连、网关和消息中继 | 厂商文档面向产品开发者，不给出本灯带的第三方控制密钥或 API |
| [小米 BLE Mesh 模组快速开始](https://iot.mi.com/v2/new/doc/quick-started/mesh-module) | 官方文档指出灯具的标准扩展程序支持蓝牙直连控制 | 具体灯带是否采用该标准扩展程序，需型号和实机确认 |
| [Xiaomi diffuser BLE](https://github.com/clairefiorettiythzr6802-eng/xiaomi-diffuser-ble) | 同属米家生态设备的 FE95 GATT 直连、认证、属性收发与 MQTT 桥接实例，值得参考其抓包和验证方法 | 仅在作者的 `xiaomi.diffuser.xw002` 上验证；不能把其 UUID、密钥类型、帧格式直接套到灯带 |
| [xiaomi-diffuser-ble 的 MIoT BLE v2 协议实测](https://github.com/clairefiorettiythzr6802-eng/xiaomi-diffuser-ble/blob/main/docs/protocol-xw002-zh.md) | 其实测 GATT 通道 `0x0010/0x0016/0x001a/0x001b/0x001c` 与本项目 ESP32 观察到的 FE95 特征集合相交；展示 `gatt_ltmk` + ECDH/HKDF/AES-CCM + MIoT 属性读写路径 | 只在 XW002 上完成实机闭环；与本灯带特征吻合是强候选证据，尚不是本灯带的认证或控制验证 |
| [esp32-blemesh2mqtt](https://github.com/ludodefgh/esp32-blemesh2mqtt) | ESP32 上的 BLE Mesh provisioner、Wi-Fi/MQTT、HA discovery 架构参考 | 目标是标准 Bluetooth Mesh；米家设备是否兼容，须实测，不能根据“Mesh”名称判断 |
| [XiaomiGateway3](https://github.com/AlexxIT/XiaomiGateway3) | 小米网关下的 BLE/Mesh 设备识别和 MIoT 属性映射资料 | 依赖现有米家网关；不是“去网关”的直接实现 |
| [XiaomiGateway3 对本型号的支持请求](https://github.com/AlexxIT/XiaomiGateway3/issues/1553) | 将 `lemesh.light.wy0d02` 映射到产品 ID `19733`，列出开关、亮度、色温等属性 | 展示的是经小米网关收发的 MIoT 属性，不是 ESP32 到灯具的蓝牙报文 |
| [本型号 MIoT 规格](https://home.miot-spec.com/spec/lemesh.light.wy0d02) | 服务 2 的开关 `2.p.1`、亮度 `2.p.2`（1–100）、色温 `2.p.3`（2700–6500 K） | 第三方索引；属性标识不能直接当作 GATT 特征或 Mesh opcode |
| [MiEcosystem MiBeacon 实现](https://github.com/MiEcosystem/mijia_ble_common/blob/master/mible_beacon.c) | FE95 service data 的帧头包含产品 ID，可用于只读扫描识别 | 广播产品 ID 只标识型号，不能证明该设备可被直接控制 |
| [通用 Xiaomi FE95 Mi Service 协议说明](https://github.com/pvvx/ATC_MiThermometer/blob/master/InfoMijiaBLE/Mijia%20BLE%20connection%20protocol-Mi%20Service%20v4.md) | 记录 FE95 GATT 中若干短 UUID 的通用用途，例如 `0x0010`/`0x0016` 与认证相关 | 第三方对通用协议的资料；不能单独证明本灯带采用相同认证流程或公开其控制帧 |
| [MiEcosystem 蓝牙 Mesh 接入开发](https://github.com/MiEcosystem/miio_open/blob/master/ble/04-%E8%93%9D%E7%89%99Mesh%E6%8E%A5%E5%85%A5%E5%BC%80%E5%8F%91.md) | 官方说明 MIoT 属性到 SIG Mesh OnOff/Lightness/CTL 模型的映射，并列出 GATT 直连透传服务 UUID | 是通用开发文档；未证明本型号按该模板实现，也未提供已配网网络密钥或本机认证密钥 |
| [XiaoMi/ha_xiaomi_home](https://github.com/XiaoMi/ha_xiaomi_home) | MIoT 设备模型、属性名称、本地接入限制的官方实现 | 面向 Home Assistant 和小米中枢网关；不能证明 ESP32 可独立接管 Mesh |
| 本机 ESP-IDF `examples/bluetooth/nimble/blecent` | 与已安装 IDF 6.0.1 一致的扫描、连接和 GATT API 用法 | 示例协议是通用 BLE，实际灯带认证仍需研究 |

## 技术判断

“米家 Mesh 2.0”这个产品名称不足以确定是 Bluetooth SIG Mesh、米家私有 Mesh，还是额外提供可直连 GATT。小米官方文档确认其 BLE Mesh 方案可以具有 GATT 直连控制，但是否开放、如何认证由具体设备决定。先收集型号和广播/GATT 证据，再选方案。

`FE95` 是米家相关服务标识。2026-10-23 ESP32 实测已连接到 PID 19733 对应的广播源，枚举出 FE95 服务以及 `0x0004`、`0x0010`、`0x0016`、`0x0005`、`0x0017`、`0x0018`、`0x001a`、`0x001b`、`0x001c` 特征。通用 FE95 Mi Service 资料可解释部分认证特征，但本次未读写。进一步对照一款 BLE Mesh 米家设备的已实测 MIoT BLE v2 通道，其 `0x0010`(控制/登录)、`0x0016`(认证数据)、`0x001a/0x001b`(加密数据)、`0x001c`(MCU信息) 与本机实测服务的 UUID 和属性高度吻合；这是本型号使用同类 MIoT BLE v2 直连路径的有力候选，仍待本型号登录成功验证。那款参考设备从小米云端 `/app/v2/blemesh/query_dev` 获取 32 字节 `gatt_ltmk`，但不能直接证明本型号也用同一云端密钥流程。

通用 Mi Service 说明，连接设备须先完成绑定或登录认证后才能进行特征读写；重新绑定成功还会清除先前用户配置。因此当前固件仍保持纯只读。`tools/fetch_gatt_ltmk.py` 是候选 v2 路径的本地密钥获取工具：通过 Xiaomi 账号登录响应临时取得 `userId/serviceToken/ssecurity`，按 MiCloud 签名请求列出账号设备并仅筛选目标型号，再取该 DID 的 key；账号密码和会话字段只在进程内使用，不回显或写盘，key 写到仓库外且设置目录 0700/文件 0600，不回显 key/DID。现已实现普通密码登录及手机/邮箱验证码流程；CAPTCHA 等登录分支仍会安全停止。尚未用用户账号调用，也没有证明目标设备接受 v2 登录。协议参考：[Mi Service v4](https://github.com/pvvx/ATC_MiThermometer/blob/master/InfoMijiaBLE/Mijia%20BLE%20connection%20protocol-Mi%20Service%20v4.md)、[XW002 MIoT BLE v2 实测](https://github.com/clairefiorettiythzr6802-eng/xiaomi-diffuser-ble/blob/main/docs/protocol-xw002-zh.md)。

## 已定位的设备

用户报告米家名称“情景 lemesh 色温灯 Ultra”，固件 `3.02_0020`。公开资料对应 `lemesh.light.wy0d02`，产品 ID `19733` (`0x4D15`)；第三方固件索引列有写作 `3.0.2_0020` 的版本，**很可能**只是显示格式差异，尚未从设备读回确认。该产品的规格是色温灯，未列出 RGB 颜色属性；实体若是灯带，仍需核对是否同一产品。型号映射和固件索引只能缩小调查范围，不能推出直连密钥或控制报文。

## 首轮诊断后的分支

- `pid=19733` 已可重复连接并枚举服务。实测没有发现官方通用文档列出的 128-bit GATT 透传服务 `00000100-0065-6c62-2e74-6f696d2e696d`；设备呈现 FE95 GATT 服务。实机通道特征与一款其他 BLE Mesh 设备已验证的 MIoT BLE v2 GATT 映射高度吻合；优先验证这条直连路线，但在登录响应 `0x51`、属性回读和灯实物动作之前，仍标记为候选，不得套用已知参考设备的属性编号或直接发命令。
- 若主要出现 `mesh_ad=0x2`：已配网节点发出标准 Mesh 加密消息，不能仅凭广播解出网络密钥。下一步核实是否可从本人账户或网关合法取得网络资料；否则单独重建 ESP32 管理的 Mesh 网络将涉及灯具解绑/重置，需先评估现有米家配置。
- 若两者都没有：核对灯具与 ESP32 距离、供电、实际设备型号和扫描窗口，不凭空选择协议。

## 已配网状态与重置

小米官方开发文档说明，米家 BLE Mesh SDK **不强制统一的重置方式**，产品开发者自行定义；照明设备“通常”快速开关 5 次，但这只是通用示例，不是 `lemesh.light.wy0d02` 的确认操作。不能据此让用户对这款灯带执行 5 次断电。重置可能清除灯具现有 Mesh 网络配置，造成它离开当前米家网络并需要重新配网。

用户手机已配对/已绑定，可能影响设备当前处于已配网还是可配网状态，但现有证据不足以证明“手机配对”会让正常 BLE 广播完全消失，也不足以解释两轮 Mac 扫描没有出现 PID 19733。手机系统的 BLE 扫描与 ESP32/NimBLE 的 Mesh 广播观察也不一定等价。先保留当前配置，烧录只读诊断固件，在灯具附近记录广播类型；可先暂时关闭手机蓝牙并退出米家 App 做对照扫描。只有确认需要重新配网、且找到该型号准确的复位说明后，再讨论重置。

来源： [小米官方 BLE Mesh 接入开发文档](https://github.com/MiEcosystem/miio_open/blob/master/ble/04-%E8%93%9D%E7%89%99Mesh%E6%8E%A5%E5%85%A5%E5%BC%80%E5%8F%91.md) 明确区分 SDK 通用规则与产品自行定义的复位方式；它并未给出 Lemesh Ultra 的具体复位步骤。
