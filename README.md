# IoT 环境监测系统 · ESP32-S3 固件

ESP32-S3 采集温湿度 → WiFi + SNTP 校时 → MQTT 上报 → 后端入库。
后端见同级目录 `../backend`（独立仓库）。

对应设计文档 `ESP32S3_IoT_AI_工程设计文档_v1.1(1).md`。当前**只剩 Phase 8（AI）**，
而它的前置条件是先积累几周真实数据（文档 §26）——所以固件与后端都已进入"跑着攒数据"阶段。

## 硬件

| 项 | 值 |
|---|---|
| 模组 | ESP32-S3-N16R8（16MB Flash + 8MB Octal PSRAM） |
| 框架 | ESP-IDF **v6.1** |
| CPU | 160 MHz |
| FreeRTOS tick | 100 Hz |
| Flash | 16MB，DIO 模式 |
| 分区表 | 工程内 `partitions.csv`：nvs `0x6000` + phy_init `0x1000` + factory **4MB** |
| 控制台 | UART0 @115200（GPIO43/44） |
| 调试 | 内置 USB-JTAG（OpenOCD `board/esp32s3-builtin.cfg`） |

不用 IDF 默认分区表的原因：`partitions_singleapp.csv` 把 factory 写死 1MB、不随
Flash 容量变化，16MB 的板子会有 14MB 用不上。`partitions.csv` 里 Offset 一列留空，
由构建系统按 4KB 对齐排布，避免手算出错。

### 接线

| 外设 | GPIO | 有效电平 | 备注 |
|---|---|---|---|
| 外接 LED | 2 | 高电平点亮 | |
| 按键 KEY2 / BOOT | 0 | 低电平有效 | strapping 脚，复位瞬间不要按住 |
| WS2812B RGB | 48 | — | 板载 1 颗，靠 RMT 外设驱动 |
| DHT11 数据线 | 6 | — | **必须外接 5.1kΩ 上拉到 3.3V** |

板载 LED1/LED2 接在 U0RXD/U0TXD 上，是串口收发指示灯，**不适合当用户 LED 用**。

S3 上不能随便用的引脚：`26-32` Flash 总线、`33-37` PSRAM 数据线（N16R8 占了 5 个）、
`19/20` USB-JTAG、`43/44` 控制台、`0/3/45/46` strapping。相对安全的是
`1、2、4-18、21、38-42、47、48`（完整清单在 `main/Kconfig.projbuild` 的 help 里）。

**DHT11 有三条硬性要求，都不是"建议"**：

1. **外接 5.1kΩ 上拉到 3.3V。** 总线是开漏的，两端都只会把线拉低，靠上拉回到高电平。
   代码里虽然启用了内部上拉兜底，但内部上拉约 45kΩ，弱近十倍，上升沿太慢会导致误码。
   不接外部上拉的典型症状是"偶尔能读到、经常校验和错误"。
2. **用 3.3V 供电，不要用 5V。** 手册的判决门限是高电平 ≥ **70% VDD**：
   VDD=5V 时阈值 3.5V，而 S3 只能输出 3.3V，正好落在不确定区。VDD=3.3V 时阈值 2.31V，余量充足。
3. **3.3V 供电时数据线不超过 100cm**（手册规定；5V 供电时可到 20m）。

另建议在传感器 VCC/GND 之间并一个 100nF 去耦电容。

## 数据流

```
按键 GPIO ──中断──▶ button 组件 ISR ──队列──▶ app/button_task
                                                    │
                                                队列（新周期）
                                                    ▼
                      led 组件 ◀── app/blink_task ◀──┘

RGB GPIO ◀── RMT 外设 ◀── rgb_led 组件 ◀── app/rgb_task（独立循环色相）

┌────────── 采样管道（一个生产者 / 两个消费者）────────────┐
│                                                          │
│  DHT11 数据线 ◀── 单总线时序 ◀── dht11 组件               │
│                                    │                     │
│                              app/sensor_task             │
│                              （只采样，绝不阻塞）          │
│                                    │                     │
│                     ┌──────────────┴──────────────┐      │
│                     ▼                             ▼      │
│            Queue console_queue          Queue mqtt_queue │
│                     │                             │      │
│             app/console_task                app/mqtt_task│
│             （格式化成 JSON 打串口）      （publish 到    │
│              不依赖网络，断网时照常打印      broker）     │
└──────────────────────────────────────────────────────────┘

WiFi   ◀── wifi_mgr 组件（Station 模式）
NTP    ◀── time_mgr 组件（SNTP，把开机毫秒锚定成 Unix 毫秒）
broker ◀── mqtt_mgr 组件（连接管理 + publish，带 LWT 在线状态）
```

### 为什么是两条队列，不是一条加两个消费者

`xQueueReceive` 是**出队**语义。两个任务读同一条队列会变成**瓜分**数据，而不是广播——
每条记录只有一个消费者拿得到。所以生产者复制一份投进两条独立队列。

两个消费者还有一个**刻意的行为差异**：`mqtt_task` 在未连接时**不消费**队列（让它积压，
重连后补发），而 `console_task` 永远在消费。于是"网络断了导致丢数据"只影响上报那一路，
串口照常一行一行地打——**两种故障因此可以被区分开**。

队列深度 8，元素是 `sensor_data_t`（见 `components/app/src/data_model.h`）。

## 软件架构

### 配置注入式分层

`main.c` 只做两件事：

1. 把板级配置（引脚号、周期表、凭据）装进 `app_config_t`
2. 调 `app_start()` 把控制权交给应用层

`components/app/` 里**不含任何"这台板子特有"的信息**——引脚接在哪、连哪个 AP、
设备叫什么，全部通过 `app_config_t` 从外部注入。所以整个 `app/` 目录可以原样复制到别的工程。

`app.c` 逐字段**手工搬运**，不用 `memcpy`：C 里布局相同的结构体也是不同类型，
强转会静默读错字段。多写几行的代价换掉一类最难查的 bug。

引脚/模式放在**运行期结构体**里而不是 `menuconfig` 里，因为 S3 有 GPIO 交换矩阵，
同一个外设可以映射到不同引脚，这些是"运行期可配"的东西；`menuconfig` 只该管编译期参数
（Flash 容量、分区表、tick 频率）。

IDF 的 `xxx_init(config)` 是**一次性输入参数**——读完就拷走，不是 HAL 那种长期 handle。
所以 `app_config_t` 里的 `const char *` 指针只需在 `app_start()` 期间有效。

### 组件

| 组件 | 职责 | 关键点 |
|---|---|---|
| `app` | 任务编排、队列、报文组装 | 不含板级信息，可整体复用 |
| `led` | 普通 GPIO LED | 有效电平可配（拉电流/灌电流两种接法） |
| `button` | 按键 + 中断 | 去抖在 ISR 里用 tick 计数做纯算术判断 |
| `rgb_led` | WS2812B | 依赖托管组件 `espressif/led_strip`，走 RMT |
| `dht11` | 单总线温湿度 | 纯 GPIO 忙等；收帧只屏蔽**本核**中断（见下） |
| `wifi_mgr` | Station 连接管理 | init 不阻塞 / `wait_connected` / 无限重连 / `get_rssi` |
| `time_mgr` | SNTP | **无独立任务**，靠 lwIP 自带定时器 |
| `mqtt_mgr` | 连接管理 + publish | 依赖 `espressif/mqtt`，带 LWT 在线状态 |
| `settings` | NVS 持久化 | 命名空间 `app_cfg`，键 `period_ms` |

依赖的托管组件（`dependencies.lock` 已入库）：`espressif/led_strip` 3.0.3、
`espressif/mqtt` 1.1.0。

> IDF v6.1 已把 esp-mqtt **移出核心**，改为托管组件。头文件仍是 `mqtt_client.h`，
> 但需要 `idf_component.yml` 声明——直接 `#include` 而没声明依赖会报 "file not found"。

### DHT11：为什么不用 `portENTER_CRITICAL()`

DHT11 是单总线器件，靠**微秒级的电平持续时间**编码数据，收帧期间必须独占 CPU。
直觉做法是 `portENTER_CRITICAL(&mux)`，但它在本工程的 Xtensa 端口上**不只是屏蔽中断**——
还要获取一把**跨核自旋锁**。而 core 0 上跑着 WiFi / lwIP 协议栈，把它冻住几毫秒
轻则丢包、重则喂不上看门狗。

所以驱动改用 `portSET_INTERRUPT_MASK_FROM_ISR()`，它只是一条 `rsil` 指令，
改的是**本核**的 `PS.INTLEVEL`，对另一个核毫无影响。这正是需要的语义：
驱动的时间基准是 `esp_cpu_get_cycle_count()`（CCOUNT），而 **CCOUNT 是每核一份的**，
另一个核跑不跑、跑多快，跟量时间没有任何关系。

临界区也切成了两段，只有该独占的那段才屏蔽：

```
Phase A（中断开启）    ：拉低总线 → vTaskDelay(20ms)
Phase B（屏蔽本核中断）：释放总线 → 等应答 → 收 40 位 → 恢复中断
```

如果把 Phase A 也塞进临界区，等于白白让整个系统的中断延迟增加 20ms。

### 任务

| 任务 | 栈（字节） | 优先级 | 职责 |
|---|---|---|---|
| `blink` | 2048 | 5 | 按当前档位闪 LED |
| `button` | 3072 | 5 | 处理按键事件，切换闪烁档位并写入 NVS |
| `rgb` | 2560 | 5 | 独立循环色相 |
| `sensor` | 2560 | 5 | 读 DHT11，**只投队列，绝不阻塞** |
| `console` | 3584 | 5 | 取队列 → 组 JSON → 打串口 |
| `mqtt` | 4096 | 5 | 取队列 → publish（未连接时不消费） |
| `monitor` | 3072 | 4 | 周期打印堆/栈水位/状态（优先级最低） |

栈的单位是**字节**（`portSTACK_TYPE = uint8_t`），不要乘 `sizeof(StackType_t)`。
监控任务会打印各任务的栈水位，余量建议留 30% 以上——中断嵌套、日志格式化、
浮点运算都可能临时吃掉更多。

### 时间设计：队列里存开机毫秒，只在发报文时换算

队列里存的**一直是开机毫秒**，直到报文即将离开设备才调
`time_mgr_boot_ms_to_unix_ms()` 换算成 Unix 毫秒。

换算方式是 `boot_ms + (当前墙钟 − 当前开机时钟)`，括号里是常量偏差。所以
**"同步前采样、同步后发出"的数据也能拿到正确的 Unix 时间**。

反过来如果在采样时就写 Unix 时间，同步前采的数据就**永久失去**了正确时间信息，
事后无法补救。

未同步时该函数原样返回 `boot_ms`，报文里 `clock` 字段标成 `"boot"`——
接收端不必靠数值大小猜时间基准。

## 上报协议

### topic

| topic | 方向 | 载荷 |
|---|---|---|
| `iot/<device_id>/telemetry` | 设备 → broker | 遥测报文（QoS 1） |
| `iot/<device_id>/status` | 设备 → broker | `{"state":"online"}` / `{"state":"offline"}`（QoS 1 + retain） |

API 只接受**后缀**，前缀由组件拼。为什么不让调用方传完整 topic：它一定会写错
——少个斜杠、`device_id` 拼错大小写、把 retain 和 qos 弄反。而拼错的 topic
**不报任何错**，只是后端永远收不到，很难查。

### 遥测报文

```json
{"device_id":"esp32s3_001","ts":1760000000000,"seq":213,"temperature_c":34,"humidity_rh":52,"wifi":true,"rssi":-58,"clock":"unix"}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_id` | string | 设备标识，长度上限 31 字节 |
| `ts` | int64 | 采样时刻。`clock=unix` 时是 Unix 毫秒 |
| `seq` | uint32 | 递增序号，用于发现丢包/静默停发 |
| `temperature_c` | int | 摄氏度（DHT11 分辨率 1℃） |
| `humidity_rh` | int | 相对湿度 % |
| `wifi` | bool | 采样时是否已连上 |
| `rssi` | int \| **null** | 未连接时是 `null`，**不是 0**——0 dBm 是合法且极强的读数 |
| `clock` | string | `"unix"` / `"boot"`，标明时间基准 |

两个刻意的设计：

- **`rssi` 无效时发 `null` 而不是省略字段。** 省略会让 schema 不稳定，接收端就得写
  "有没有这个键"的分支；恒发键、值可为 `null`，schema 就固定了。
- **自己拼字符串，不用 cJSON。** 报文只有八个字段、结构固定，一次 `snprintf` 就写完。
  引入 JSON 库要多占几 KB Flash，还要在堆上分配/释放——为了八个字段付这个代价不划算，
  而且多一层堆分配就多一种"分配失败"的失败模式。

### 在线状态与 LWT

`mqtt_mgr_init()` 通过 `session.last_will` 设置**遗嘱消息**：
topic `iot/<device_id>/status`、payload `{"state":"offline"}`、QoS 1 + retain。
这条消息由 **broker 保管**，设备异常掉线时由 broker 代发（掉电、断网、任务卡死都算，
只要 TCP 被 broker 判定断开）。连上时设备主动发 `{"state":"online"}`。

于是后端只需订阅 `status` 一个 topic 就能知道设备在不在线，不需要自己维护
"多久没收到数据就认为掉线"的超时逻辑。这是 MQTT 相比 HTTP 上报最实质的优势。

## 构建与烧录

### 前置

ESP-IDF v6.1 + esp32s3 工具链。

**Windows 上必须用 PowerShell 点源环境脚本，不能用 Git Bash**——Git Bash 下
`export.sh` 会直接报 `MSys/Mingw is not supported`：

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

EIM 安装的路径不是标准位置，需要的话显式设置（点源上面那个脚本会自动设好）：

```
IDF_PATH=D:\esp\.espressif\v6.1\esp-idf
IDF_TOOLS_PATH=C:\Espressif\tools
IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
```

### 凭据：只写本机，不入库

`WIFI_SSID` / `WIFI_PASSWORD` / `MQTT_BROKER_URI` 在 `sdkconfig.defaults` 里
**故意留空**（那个文件入库），真实值写在 `sdkconfig.secret`（已 gitignore）。

```powershell
$env:SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.secret'
idf.py build
```

**`sdkconfig.secret` 不会自动生效**，必须显式设上面那个环境变量。

事故背景：删掉 `sdkconfig` 重建后（换机器、`fullclean`、排 kconfig 问题），
Kconfig 只会读 `sdkconfig.defaults`，而那里凭据是空的——于是 WiFi 和 MQTT
**静默失效**：设备照常跑、LED 照常闪，只是不上报。串口里唯一的线索是
monitor 那行 `MQTT disabled`。

> **`sdkconfig.defaults` 里绝不能写明文凭据。** 而且"改掉文件"从来不是终点——
> 删除凭据的那个提交本身也会把明文写进自己的 diff，历史里仍然翻得出来。
> 推送前必须扫：`git log -p <range> | grep -c '<凭据>'`。

### 编译 / 烧录 / 监视

```powershell
idf.py set-target esp32s3      # 首次；之后由 sdkconfig.defaults 固定
idf.py build
idf.py -p COM9 flash monitor
```

串口是 **COM9**（USB-Serial-JTAG 的 MI_00，驱动改绑 `usbser.sys`；MI_02 保持
WinUSB 供 JTAG 用）。**串口与 JTAG 是两条独立通路**，可以一边 monitor 一边调试。

### 四个会阻断构建的坑

**① `sdkconfig` 不入库，`sdkconfig.defaults` 也不覆盖已存在的 `sdkconfig`。**
改完 `sdkconfig.defaults` 必须删掉 `sdkconfig` 再构建才生效（`idf.py fullclean` 或直接删文件）。

**② ccache 会让 ninja 报 `CreateProcess failed`。** 加 `-DCCACHE_ENABLE=0`
并设 `IDF_CCACHE_ENABLE=0`。

**③ 编译器临时文件 `Permission denied`。** 把 `TMP` / `TEMP` 指到工作区内的目录。

**④ 全量并行编译会触发 GCC `internal compiler error: Segmentation fault`。**
实测崩在 IDF 自带的 `esp_lcd_panel_rgb.c`（RTL pass `ira`），与本工程代码无关——
根因是 `sdkconfig` 变动会让一批依赖 `sdkconfig.h` 的文件全量重编，20 核默认并行度下内存压力过大。

对策是降并行度。注意 **`idf.py` 不接受 `-j`**（报 `No such option '-j'`），
要在 `build/` 下直接调 ninja：

```powershell
cd build
C:\Espressif\tools\ninja\1.12.1\ninja.exe -j 8 all
```

## 目录结构

```
main/
├── main.c                板级配置 + 一行 app_start()（换板子只改这里）
└── Kconfig.projbuild     板级配置项（menuconfig 顶层，不在 Component config 下）
components/
├── app/                  任务编排
│   ├── include/app.h         公开接口：app_config_t + app_start()
│   └── src/
│       ├── app.c             初始化外设 → 建队列 → 创建任务
│       ├── app_internal.h    私有：任务原型、app_ctx_t、任务编号枚举
│       ├── data_model.h      ★ 任务间契约（sensor_data_t）
│       ├── *_task.c          七个任务各一个文件
│       └── telemetry_json.c  报文组装
├── led/ button/ rgb_led/ dht11/     外设驱动
├── wifi_mgr/ time_mgr/ mqtt_mgr/    网络
└── settings/                        NVS
partitions.csv            分区表（factory 4MB）
sdkconfig.defaults        入库的关键配置（凭据留空）
sdkconfig.secret          本机凭据（gitignore，需显式加载）
dependencies.lock         托管组件版本锁（入库）
```

加新任务的流程：`*_task.c` 写实现 → `app_internal.h` 加枚举值 → `app.c` 加一次
`xTaskCreate()`。忘了改枚举会因数组越界**编译报错**，比"静默少监控一个任务"强。

## 运行观察

`monitor` 任务每 10 秒打两行（周期见 `CONFIG_MONITOR_INTERVAL_MS`）：

- 堆占用、各任务栈水位
- WiFi 信号、MQTT 状态、采样计数、复位原因

为什么需要它：嵌入式最难查的三类问题是**死机、重启、内存不足**，它们的共同点是
"发生时你不在现场"。等发现设备不对劲时现场已经没了，所以必须在还正常的时候就
周期性留下这些数字，事后才能对着时间轴找出异常从什么时候开始。

正常启动的串口输出大致是：

```
I (xxx) main: WiFi 初始化完成，等待连接
I (xxx) wifi_mgr: WiFi 驱动已启动，发起连接
I (xxx) wifi_mgr: 已连接，IP: 192.168.1.100
I (xxx) time_mgr: 时间已同步：Unix 1760000000 秒（此后报文时间戳为 Unix 毫秒）
I (xxx) mqtt_mgr: MQTT 已连接
I (xxx) console: {"device_id":"esp32s3_001",...}
```

排障时先看这几行：`时间已同步` 不出现 = NTP 不可达；`MQTT 已连接` 不出现 =
broker 地址或网络不通；console 有报文但后端收不到 = 链路上游的问题，不是采集的问题。

## 已知的坑

| 现象 | 根因 |
|---|---|
| 设备能 ping 通，broker 日志里一条连接记录都没有 | Windows 防火墙 `RemoteIP` 没放行（默认**丢弃**不回 RST，设备端只看到 TCP 超时）。**换路由器后必须同时改防火墙和固件里的 broker 地址**，只改一处现象完全一样 |
| 换路由器后 WiFi 连上了但一条数据都发不出去 | `CONFIG_MQTT_BROKER_URI` 还是旧网段的地址，改完**必须重新烧录** |
| 改配置后 menuconfig 打不开 | 用了 `idf.sdkconfigDefaults`（已实测撤掉，扩展会把相对路径传给 build/ 下的 confserver，解析不到） |
| 按键去抖填 1~9 完全不生效 | `pdMS_TO_TICKS` 向下取整，tick=100Hz 时算出 0 tick。Kconfig 的 range 下限已设成 10 挡住 |
| DHT11 采样周期填 <2000 拿到旧值 | 读太快**不会报错**，传感器返回上一次的数据或垃圾值。range 下限已设成 2000 挡住 |
| 两个设备轮流掉线，日志只有一条语焉不详的 DISCONNECTED | 两台设备用了同一个 MQTT `client_id`，后连上的把先连上的踢下线。默认用 `device_id` 当 `client_id` 就是为了避免这个 |

## 状态

固件侧功能完整：LED / 按键（NVS 记忆档位）/ WS2812B / DHT11 / WiFi /
SNTP / MQTT / 系统监控，全部跑通。

**还没做**：

| 项 | 文档 | 说明 |
|---|---|---|
| AI（异常检测 / 预测） | §26-§33 | 前置条件是先积累几周真实数据 |
| TLS / 鉴权 | §39 | 当前是明文 `mqtt://` + `allow_anonymous`；正式部署再上 `mqtts://` 8883 + 证书 |
| OTA | — | 分区表已预留改造空间（`factory` 换成 `ota_0`/`ota_1` + `otadata` 即可） |

**AI 不是"还没写"，是"还不能写"**：文档 §26 的第一原则是"先有数据，再训练模型"。
