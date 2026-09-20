/*
 * dht11.c —— DHT11 单总线温湿度驱动实现
 *
 * ════════════════════════════════════════════════════════════════════
 * 一、这个驱动真正难的地方不是"协议"，而是"时间"
 * ════════════════════════════════════════════════════════════════════
 *
 * DHT11 没有时钟线，它用高电平**持续多久**来编码 0 和 1：
 *
 *     0 位：低约 50µs + 高 26~28µs
 *     1 位：低约 50µs + 高 70µs
 *
 * 判位分界线大约在 50µs。整帧 40 位，加上应答信号，收帧窗口只有约 4ms。
 * 如果这 4ms 里被中断打断，被打断那一位的采样时刻就会漂移，
 * 轻则读错一位，重则整帧作废。所以收帧期间必须屏蔽中断。
 *
 *
 * ════════════════════════════════════════════════════════════════════
 * 二、为什么分成两个阶段（Phase A / Phase B）
 * ════════════════════════════════════════════════════════════════════
 *
 * 一次读取总共约 24ms：
 *
 *     主机起始信号（手册要求 >= 18ms，本驱动用 20ms）   ≈ 20ms
 *     从机应答 + 40 位数据                            ≈  4ms
 *
 * 只有后面那 4ms 需要屏蔽中断。前面 20ms 的起始信号完全可以用
 * vTaskDelay() 在中断开启的状态下慢慢等——把这一段也塞进临界区，
 * 等于白白让整个系统的中断延迟增加 20ms，纯属自找麻烦。
 *
 *      Phase A（中断开启）        ：拉低总线 → vTaskDelay(20ms)
 *      Phase B（屏蔽本核中断）    ：释放总线 → 等应答 → 收 40 位 → 恢复中断
 *
 *
 * ════════════════════════════════════════════════════════════════════
 * 三、为什么用 portSET_INTERRUPT_MASK_FROM_ISR()
 *     而不是 portENTER_CRITICAL()
 * ════════════════════════════════════════════════════════════════════
 *
 * 这是本驱动最关键的一个取舍，必须说透。
 *
 *   portENTER_CRITICAL(&mux) 在本工程的 Xtensa 端口上展开为
 *   vPortEnterCritical(mux) → xPortEnterCriticalTimeout(mux, ...)
 *   （见 freertos/FreeRTOS-Kernel/portable/xtensa/port.c）。
 *   它除了用 rsil 屏蔽本核中断之外，还要**获取一把跨核自旋锁**：
 *
 *       if (!spinlock_acquire(mux, timeout)) { ... }
 *
 *   本芯片是双核 ESP32-S3。自旋锁被一个核拿住时，另一个核再来拿
 *   只能原地空转。也就是说，portENTER_CRITICAL() 会把**两个核一起**拖住。
 *   而 core 0 上跑着 WiFi / lwIP 协议栈——把它冻住 4ms，
 *   轻则丢包、重则喂不上看门狗。这是绝对不能接受的。
 *
 *   portSET_INTERRUPT_MASK_FROM_ISR() / portCLEAR_INTERRUPT_MASK_FROM_ISR()
 *   则只是 XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL)——一条 rsil 指令，
 *   改的是**本核**的 PS.INTLEVEL 寄存器，对另一个核毫无影响。
 *   这正是我们要的语义：本驱动的时间基准是 CCOUNT
 *   （esp_cpu_get_cycle_count()），而 CCOUNT 是"每核一份"的，
 *   另一个核跑不跑、跑多快，跟我们量时间没有任何关系。
 *
 *   名字里的 "FROM_ISR" 只是 FreeRTOS 的历史命名。它的实现是纯寄存器操作，
 *   在普通任务上下文里调用同样正确、同样安全；区别只是它不会去动
 *   "临界区嵌套计数"那套东西——而我们也不需要，因为屏蔽窗口内
 *   不调用任何 FreeRTOS API。
 *
 * 残余风险（必须知道）：
 *
 *   1. 本核中断会被关闭约 4ms。tick 频率是 100Hz（10ms 一个 tick），
 *      4ms < 10ms，所以最坏情况只是把某个 tick 推迟，不会丢 tick。
 *      但其他中断（GPIO、串口等）会被推迟最多 4ms；若系统里有
 *      对中断延迟敏感的外设，请先评估再决定用这个驱动还是改用 RMT。
 *
 *   2. 屏蔽窗口内只调用 gpio_ll_get_level() / gpio_ll_set_level()。
 *      这两个函数是 __attribute__((always_inline)) 的**纯寄存器读写**
 *      （见 esp_hal_gpio/esp32s3/include/hal/gpio_ll.h），
 *      会被内联进调用点，不存在"取指要访问 Flash"的问题。
 *
 *      **如果换成 gpio_get_level()，它是住在 Flash 里的普通函数**——
 *      一旦此时另一个核正在擦写 Flash（Cache 被禁用），本核取指就会崩。
 *      这是不能用 gpio_get_level() 的硬性原因，不是风格偏好。
 *
 *
 * ════════════════════════════════════════════════════════════════════
 * 四、并发调用怎么处理：忙标志，而不是把互斥量握一整帧
 * ════════════════════════════════════════════════════════════════════
 *
 * 直觉做法是"进函数就 portENTER_CRITICAL(&s_read_mux)，出函数再释放"，
 * 这样两个任务绝不会交叉收帧。但按第三节的分析，那等于把跨核自旋锁
 * 连续握住 24ms，又把两个核一起冻住了——为了解决一个小概率问题，
 * 制造了一个必然发生的大问题。
 *
 * 所以本驱动只在**检查/设置一个忙标志的那几十纳秒**里持锁：
 *
 *      portENTER_CRITICAL(&s_read_mux);
 *      if (s_busy) { ...释放锁并立刻失败... }
 *      s_busy = true;
 *      portEXIT_CRITICAL(&s_read_mux);
 *
 * 之后的 20ms+4ms 收帧过程完全不再碰这把锁。效果是：
 *
 *   - 两个任务同时调用时，后来的那个**当场**拿到 ESP_ERR_INVALID_STATE
 *     并立即返回，而不是把总线搅成两段交错的时序。后者会读出垃圾，
 *     而且校验和还可能是对的——属于最难排查的一类故障。
 *   - 跨核锁的持有时间从 24ms 降到几十纳秒。
 *
 * "快速失败"比"排队等待"更适合这个场景：温湿度是慢变量，
 * 调用方拿到一个明确的错误码，自己决定要不要重试即可。
 *
 *
 * ════════════════════════════════════════════════════════════════════
 * 五、为什么"查校验和"远远不够
 * ════════════════════════════════════════════════════════════════════
 *
 * 这是本驱动和很多现成实现的分水岭。
 *
 *     全 0 帧  00 00 00 00 00：校验和 (0+0+0+0)&0xFF = 0x00 == 0x00  ✓ 通过
 *     全 1 帧  FF FF FF FF FF：校验和 (0xFF×4)&0xFF = 0xFF == 0xFF  ✓ 通过
 *
 * 这两种最典型的故障帧**都能通过校验和**。传感器掉线、上拉电阻忘接、
 * 杜邦线太长导致边沿畸变、或者判位逻辑写错位置，都会让采样结果
 * 一律是全 0 或一律是全 1。只查校验和的驱动会把"0°C / 0%RH"
 * 当成一次成功读数交给上层——这就是 DHT 驱动里最经典的潜伏 bug。
 *
 * 本驱动的防线有三层，而且**顺序不能反**：
 *
 *   1. 逐位的"位宽合法性 + 判位法互证"——在收帧过程中就做掉。
 *      - 位宽合法性：合法的高电平要么 <= 30µs（0 位），要么 >= 68µs（1 位）；
 *        落在 (30, 68)µs 这个中间地带的一律判为非法——因为手册保证
 *        不会出现这种宽度，出现了就说明时序已被破坏。
 *      - 判位法互证：方法 A 在 50µs 处读电平，方法 B 量完整高电平宽度，
 *        方法 C 比较高电平与低电平的宽度。一个位不可能既"在 50µs 时
 *        还是高"又"高电平宽度小于 50µs"，三者矛盾就说明这一位的
 *        时序已不可信，整帧作废。
 *      这一层之所以必须放在最前面，是因为它是**唯一**能识破
 *      全 0 / 全 1 这类"校验和自洽的垃圾帧"的手段。
 *
 *   2. 校验和 (hum_int + hum_dec + temp_int + temp_dec) & 0xFF。
 *      它挡的是"位翻转"型错误——这一层很有效，只是挡不住全 0/全 1。
 *
 *   3. 量程检查：湿度 0~100%，温度 0~50°C。
 *      挡的是解出来的数值本身不可能是 DHT11 的读数的情况。
 *
 *   关于位宽上界为什么取 30µs 而不是更紧的 27µs：
 *      市面流传的 DHT11 手册有两个互相矛盾的版本——电气特性表写
 *      TH0 = 26~28µs，而时序图画的是 23~27µs。既然连原厂文档都不自洽，
 *      就该取**偏松**的那个上界：上界松了，一个 28µs 的合法 0 位会被
 *      判为合法（随后仍要过判位法互证 + 校验和 + 量程三道关），
 *      上界紧了，合法帧会被整帧丢掉，表现为"传感器时好时坏"。
 *      两种错误的代价完全不对称，所以选 30。
 *
 *
 * ════════════════════════════════════════════════════════════════════
 * 六、时间基准：为什么用 CCOUNT 而不用 esp_clk_cpu_freq()
 * ════════════════════════════════════════════════════════════════════
 *
 * esp_clk_cpu_freq() 声明在 esp_private/esp_clk.h 里，而 esp_private/ 目录
 * **不在**本工程的包含路径上（可以在 build/compile_commands.json 里
 * 搜 "esp_private" 得到验证：一条 -I 都没有）。那是 IDF 内部的私有头文件，
 * 用了会直接编译不过。
 *
 * 所以这里用编译期常量 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 换算。
 * 这样做成立有一个前提：**CPU 频率固定不变**。
 * 本工程 CONFIG_PM_ENABLE 未打开（没有 DFS 动态调频），CPU 恒为 160MHz，
 * 所以"周期数 ÷ 160"恒等于微秒数。
 * 哪天开了 DFS，这个换算就会随频率漂移而失效——那时必须改成运行时读频率，
 * 并给本组件加上 esp_pm 依赖。
 */
#include "dht11.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

/* 日志标签。约定用组件名，方便在串口输出里过滤。 */
static const char *TAG = "dht11";

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

/* 未初始化的哨兵。用 -1 而不是 0，因为 GPIO0 是合法引脚。 */
#define DHT11_GPIO_UNINIT          (-1)

/* 主机起始拉低时间。DHT11 手册要求 >= 18ms；
 * AM2302（DHT22）手册里的 Tbe 上限是 20ms，取 20 兼顾两者。 */
#define DHT11_START_LOW_MS         20

/* 等从机把总线拉低的上限。
 * 手册 Tgo max = 200µs。注意 esp-idf-lib 这里用的是 40µs——
 * 那是它的一个已知 bug：慢一点但完全符合手册的传感器会被误判为"无应答"。 */
#define DHT11_RESPONSE_TIMEOUT_US  200

/* 等应答脉冲、以及等每一位电平跳变的上限。
 * 手册 Trel / Treh max = 85µs，留一点余量取 90µs。 */
#define DHT11_PHASE_TIMEOUT_US     90

/* 0/1 判位阈值。
 *
 * 合法的高电平宽度是 [0, 30]µs（0 位）∪ [68, +∞)µs（1 位），
 * 50µs 落在两段之间，且离两侧边界分别有 20µs / 18µs 的余量——
 * 比几何中点 49µs 更好记，余量也更均衡。
 *
 * 用法 A 是"忙等到这一时刻读电平"，用法 B 是"宽度大于它就判 1"。 */
#define DHT11_BIT_SAMPLE_US        50

/* 0 位高电平的上界（手册 TH0 max）。
 * 为什么是 30 而不是更紧的 27，见文件头第五节的代价分析。 */
#define DHT11_BIT0_MAX_US          30

/* 1 位高电平的下界（手册 TH1 min）。 */
#define DHT11_BIT1_MIN_US          68

/* 每微秒的 CPU 周期数。
 * 前提见文件头第六节：CONFIG_PM_ENABLE 未打开，频率恒为 160MHz。 */
#define DHT11_CYCLES_PER_US        (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)

/* 一帧的位数与字节数：湿度整数、湿度小数、温度整数、温度小数、校验和。 */
#define DHT11_BIT_COUNT            40
#define DHT11_BYTE_COUNT           5

/* 量程。DHT11 手册规定：湿度 20~90%RH，温度 0~50°C。
 *
 * 这里的湿度校验刻意放宽到 0~100：克隆件实际输出 0 和 100 很常见，
 * 卡在 20~90 会把正常读数误判成"数据非法"。
 * 真正拦住垃圾帧的是上面的**位宽合法窗口 + 判位法互证**，
 * 不是这个范围检查——范围检查只负责挡住明显离谱的值。 */
#define DHT11_HUMIDITY_MIN         0
#define DHT11_HUMIDITY_MAX         100
#define DHT11_TEMPERATURE_MIN      0
#define DHT11_TEMPERATURE_MAX      50

/* 手册要求的最小读取间隔（毫秒）。Kconfig 会禁止比这更小的配置，
 * 这里再兜一层，防止有人绕过 Kconfig 直接调 API。 */
#define DHT11_MIN_INTERVAL_FLOOR_MS 2000

/* ------------------------------------------------------------------ */
/* 静态状态                                                            */
/* ------------------------------------------------------------------ */

static int s_gpio_num = DHT11_GPIO_UNINIT;
static uint32_t s_min_interval_ms;
static int64_t s_last_read_us;   /* 0 表示"从未成功读取过"，见 dht11_read() */

/*
 * 只用来保护下面的 s_busy 标志，**绝不**跨越收帧过程。
 * 为什么不能用它把整个 dht11_read() 串起来，见文件头第四节的推导。
 */
static portMUX_TYPE s_read_mux = portMUX_INITIALIZER_UNLOCKED;

/* 忙标志：非 0 表示已有一次 dht11_read() 在收帧。 */
static bool s_busy;

/* ------------------------------------------------------------------ */
/* 时序辅助                                                            */
/* ------------------------------------------------------------------ */

/* µs → CPU 周期数。见文件头第六节关于前提条件的说明。 */
static inline uint32_t us_to_cycles(uint32_t us)
{
    return us * DHT11_CYCLES_PER_US;
}

/*
 * 忙等到相对某个 CCOUNT 快照的偏移。
 *
 * 用无符号差值，天然抗 2^32 回绕：160MHz 下 CCOUNT 大约 26.8 秒回绕一次，
 * 而一次帧只有 5ms，远小于回绕周期，所以 (now - start) 永远得到真实差值。
 * 如果用有符号比较（now < start + cycles），回绕的那一刻就会误判。
 */
static inline void busy_until(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < cycles) {
        /* 空循环。这里就是要"什么都不做，只消耗时间" */
    }
}

/*
 * 忙等直到引脚电平变成 level，或从 start 算起超过 timeout_us。
 *
 * 每个等待循环都必须有这样一个基于 CCOUNT 的截止时间。
 * 传感器掉线、线被短路、或者从机中途复位，都会让某个电平永远不出现；
 * 没有截止时间的 while 循环会把整个任务永久挂死在这儿，
 * 而它可能正持有着一把跨核锁——那就等于把整块板子搞死。
 *
 * @return true 表示等到了；false 表示超时
 */
static inline bool wait_for_level(gpio_dev_t *hw, uint32_t pin, int level,
                                  uint32_t start, uint32_t timeout_us)
{
    const uint32_t limit = us_to_cycles(timeout_us);
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < limit) {
        if (gpio_ll_get_level(hw, pin) == level) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 收帧的失败诊断                                                      */
/* ------------------------------------------------------------------ */

/*
 * read_frame() 出错的阶段。
 * 之所以要把它带出来而不是在里面直接打日志：收帧时本核中断是屏蔽的，
 * 而日志（UART 输出）非常慢，在屏蔽窗口里打日志会把"中断关闭时间"
 * 从 4ms 拉长到好几毫秒，可能丢掉别的中断。
 * 所以 read_frame() 只负责填诊断信息，日志统一由调用方在
 * portCLEAR_INTERRUPT_MASK_FROM_ISR() 之后输出。
 */
typedef enum {
    FRAME_STAGE_OK = 0,       /*!< 没有出错 */
    FRAME_STAGE_RESP_LOW,     /*!< 等从机把总线拉低（应答起始）超时 */
    FRAME_STAGE_RESP_HIGH,    /*!< 等应答的 80µs 低电平结束超时 */
    FRAME_STAGE_RESP_LOW2,    /*!< 等应答的 80µs 高电平结束超时 */
    FRAME_STAGE_BIT_RISE,     /*!< 等某一位的高电平上升沿超时 */
    FRAME_STAGE_BIT_FALL,     /*!< 等某一位的高电平下降沿超时 */
    FRAME_STAGE_BIT_WINDOW,   /*!< 某一位的高电平宽度落在非法窗口内 */
    FRAME_STAGE_BIT_CONFLICT, /*!< 判位法之间结论冲突 */
} frame_stage_t;

/** read_frame() 的诊断输出 */
typedef struct {
    frame_stage_t stage;    /*!< 出错阶段 */
    int           bit;      /*!< 出错的位序号；不在 40 位循环里时为 -1 */
    uint32_t      high_us;  /*!< 该位的高电平宽度（微秒） */
    uint32_t      low_us;   /*!< 该位之前的低电平宽度（微秒） */
    int           method_a; /*!< 采样法判出的位值，-1 表示未参与 */
    int           method_b; /*!< 宽度法判出的位值，-1 表示未参与 */
    int           method_c; /*!< 高低比较法判出的位值，-1 表示未参与 */
} frame_diag_t;

/* ------------------------------------------------------------------ */
/* 收帧                                                                */
/* ------------------------------------------------------------------ */

/*
 * 读一帧 40 位。
 *
 * 调用前提：已经从 Phase A 出来（总线被拉低过 >= 18ms），
 *           且调用方已经屏蔽了本核中断。本函数内部不调用任何
 *           会阻塞或会打日志的东西。
 *
 * @param bytes           输出 5 个原始字节
 * @param out_min_high_us 输出本帧观察到的最小高电平宽度（µs），供标定用
 * @param out_max_high_us 输出本帧观察到的最大高电平宽度（µs），供标定用
 * @param diag            输出失败诊断；成功时 stage 为 FRAME_STAGE_OK
 * @return
 *      - ESP_OK                   收到完整且时序合法的 40 位
 *      - ESP_ERR_TIMEOUT          应答缺失，或某一位的跳变没在期限内出现
 *      - ESP_ERR_INVALID_RESPONSE 位宽落在非法窗口，或判位法之间结论冲突
 */
static esp_err_t read_frame(uint8_t *bytes, uint32_t *out_min_high_us,
                            uint32_t *out_max_high_us, frame_diag_t *diag)
{
    gpio_dev_t *hw = GPIO_LL_GET_HW(0);
    const uint32_t pin = (uint32_t)s_gpio_num;
    uint8_t frame[DHT11_BYTE_COUNT] = { 0 };
    uint32_t min_high_us = UINT32_MAX;
    uint32_t max_high_us = 0;

    diag->stage = FRAME_STAGE_OK;
    diag->bit = -1;
    diag->high_us = 0;
    diag->low_us = 0;
    diag->method_a = -1;
    diag->method_b = -1;
    diag->method_c = -1;

    /*
     * 释放总线（开漏下写 1 = 高阻，由外部上拉把线拉到高），
     * 把控制权交给从机。从机看到这个上升沿之后开始应答。
     */
    gpio_ll_set_level(hw, pin, 1);

    /*
     * 等应答：从机应当在 Tgo（<= 200µs）内把线拉低。
     *
     * 这里用 200µs 而不是 esp-idf-lib 的 40µs，是因为手册明确写着
     * Tgo 最大可达 200µs。用 40µs 会让一批"慢但完全符合手册"的
     * 传感器被误判成无应答——那是驱动错，不是硬件坏。
     */
    uint32_t t0 = esp_cpu_get_cycle_count();
    if (!wait_for_level(hw, pin, 0, t0, DHT11_RESPONSE_TIMEOUT_US)) {
        diag->stage = FRAME_STAGE_RESP_LOW;
        return ESP_ERR_TIMEOUT;
    }

    /* 等应答的低电平结束（手册 80µs） */
    t0 = esp_cpu_get_cycle_count();
    if (!wait_for_level(hw, pin, 1, t0, DHT11_PHASE_TIMEOUT_US)) {
        diag->stage = FRAME_STAGE_RESP_HIGH;
        return ESP_ERR_TIMEOUT;
    }

    /* 等应答的高电平结束（手册 80µs）。这个下降沿就是第 0 位低电平期的开始。 */
    t0 = esp_cpu_get_cycle_count();
    if (!wait_for_level(hw, pin, 0, t0, DHT11_PHASE_TIMEOUT_US)) {
        diag->stage = FRAME_STAGE_RESP_LOW2;
        return ESP_ERR_TIMEOUT;
    }

    uint32_t t_low_start = esp_cpu_get_cycle_count();

    for (int i = 0; i < DHT11_BIT_COUNT; i++) {
        /* ---- 1) 等这一位的高电平上升沿 ---- */
        if (!wait_for_level(hw, pin, 1, t_low_start, DHT11_PHASE_TIMEOUT_US)) {
            diag->stage = FRAME_STAGE_BIT_RISE;
            diag->bit = i;
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t t_rise = esp_cpu_get_cycle_count();

        /* 这一位之前的低电平宽度，供"高低比较法"使用。
         * 对 DHT11 来说它应该恒为 50µs 左右（手册 TLOW min = 48µs）。 */
        const uint32_t low_us =
            (uint32_t)(t_rise - t_low_start) / DHT11_CYCLES_PER_US;

        /*
         * ---- 2) 抓下降沿（方法 B 的原始数据）----
         *
         * 关键：这个"等下降沿"的期限只能是**采样点（50µs）**，不能更长。
         *
         * 为什么？因为 0 位的高电平只有 26µs 左右。
         * 如果先老老实实忙等到 50µs 再去"等下降沿"，那时线早已是低电平，
         * 等待会**立刻返回**，把 50µs 记成高电平宽度——方法 B 就此报废，
         * 而且会把 0 位误判成需要重新检查的状态。
         * 这是本项目最容易写错的一处，教科书式的"先采样、再量宽度"
         * 顺序在这里是错的。
         */
        uint32_t t_fall;
        int level_a;

        if (wait_for_level(hw, pin, 0, t_rise, DHT11_BIT_SAMPLE_US)) {
            /* 高电平在采样点之前就结束了：先把下降沿时刻记下来 */
            t_fall = esp_cpu_get_cycle_count();

            /* 方法 A：仍然忙等到采样点，读一次**真实电平**作为独立证据。
             * 此刻线应当还是低的（0 位的低电平期有 50µs），
             * 所以这一步期望读到 0；读到 1 就说明线在这中间又跳了。 */
            busy_until(t_rise, us_to_cycles(DHT11_BIT_SAMPLE_US));
            level_a = (gpio_ll_get_level(hw, pin) != 0) ? 1 : 0;
        } else {
            /* 到了采样点还是高电平：这一位必然是 1，只需再等它落下来 */
            level_a = 1;

            /* 注意截止时间仍然从 t_rise 起算，是绝对期限 */
            if (!wait_for_level(hw, pin, 0, t_rise, DHT11_PHASE_TIMEOUT_US)) {
                diag->stage = FRAME_STAGE_BIT_FALL;
                diag->bit = i;
                diag->low_us = low_us;
                return ESP_ERR_TIMEOUT;
            }
            t_fall = esp_cpu_get_cycle_count();
        }

        const uint32_t high_us =
            (uint32_t)(t_fall - t_rise) / DHT11_CYCLES_PER_US;

        /*
         * ---- 3) 三种判位结论 ----
         *
         * 方法 A（采样法）：在 t_rise + 50µs 处读到的电平。
         * 方法 B（宽度法）：把整个高电平宽度和 50µs 比。
         * 方法 C（高低比较法）：高电平宽度和低电平宽度比大小。
         *
         * 方法 C 的依据同样来自手册：
         *     0 位高电平 TH0 max = 30µs，而每位之前的 TLOW min = 48µs；
         *     1 位高电平 TH1 min = 68µs，明显大于 TLOW。
         * 所以"高 > 低"是纯数据手册就能给出的判据，不依赖任何经验阈值。
         */
        const int method_b = (high_us >= DHT11_BIT_SAMPLE_US) ? 1 : 0;
        const int method_c = (high_us > low_us) ? 1 : 0;

        /*
         * 合法窗口检查（第一层防线）。
         * 手册保证高电平宽度要么 <= 30µs 要么 >= 68µs，中间那段是"不可能区间"。
         * 落进去说明时序已经被破坏（边沿畸变、被抢占、线太长），
         * 这一帧不可信——立刻作废，不要试图"猜"它是 0 还是 1。
         */
        if (high_us > DHT11_BIT0_MAX_US && high_us < DHT11_BIT1_MIN_US) {
            diag->stage = FRAME_STAGE_BIT_WINDOW;
            diag->bit = i;
            diag->high_us = high_us;
            diag->low_us = low_us;
            diag->method_a = level_a;
            diag->method_b = method_b;
            diag->method_c = method_c;
            return ESP_ERR_INVALID_RESPONSE;
        }

        /*
         * 判位法互证（第一层防线）。
         * 一个位不可能既"在 50µs 时还是高"又"高电平宽度不足 50µs"。
         * 出现矛盾说明采样窗口里有毛刺或者被干扰，整帧作废。
         * 这才是识破全 0 / 全 1 垃圾帧的关键——校验和对它们无能为力。
         */
        if (level_a != method_b || level_a != method_c) {
            diag->stage = FRAME_STAGE_BIT_CONFLICT;
            diag->bit = i;
            diag->high_us = high_us;
            diag->low_us = low_us;
            diag->method_a = level_a;
            diag->method_b = method_b;
            diag->method_c = method_c;
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* ---- 4) 记入本帧：MSB 先出 ---- */
        if (level_a != 0) {
            frame[i / 8] |= (uint8_t)(0x80U >> (i % 8));
        }

        /* 顺手记录高电平宽度的极值，供标定/比对逻辑分析仪用 */
        if (high_us < min_high_us) {
            min_high_us = high_us;
        }
        if (high_us > max_high_us) {
            max_high_us = high_us;
        }

        /* 这一位的下降沿就是下一位低电平期的开始 */
        t_low_start = t_fall;
    }

    for (int i = 0; i < DHT11_BYTE_COUNT; i++) {
        bytes[i] = frame[i];
    }
    *out_min_high_us = min_high_us;
    *out_max_high_us = max_high_us;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 失败诊断的日志输出                                                  */
/* ------------------------------------------------------------------ */

/*
 * 这个函数必须在恢复中断之后调用。
 * 放在这里而不是 read_frame() 里，是因为它内部会写 UART——
 * 在中断屏蔽窗口里写 UART 会把关闭中断的时间从 4ms 拉长到好几毫秒。
 */
static void log_frame_error(const frame_diag_t *diag)
{
    switch (diag->stage) {
    case FRAME_STAGE_RESP_LOW:
        ESP_LOGE(TAG, "传感器无应答：释放总线后 %u µs 内没等到低电平，"
                      "先检查 DATA 是否接对、有没有 5.1kΩ 上拉",
                 (unsigned)DHT11_RESPONSE_TIMEOUT_US);
        break;

    case FRAME_STAGE_RESP_HIGH:
        ESP_LOGE(TAG, "应答信号异常：从机拉低后 %u µs 内没有回到高电平",
                 (unsigned)DHT11_PHASE_TIMEOUT_US);
        break;

    case FRAME_STAGE_RESP_LOW2:
        ESP_LOGE(TAG, "应答信号异常：从机的应答高电平超过 %u µs 还没结束",
                 (unsigned)DHT11_PHASE_TIMEOUT_US);
        break;

    case FRAME_STAGE_BIT_RISE:
        ESP_LOGE(TAG, "收帧超时：第 %d 位的高电平上升沿在 %u µs 内没有出现",
                 diag->bit, (unsigned)DHT11_PHASE_TIMEOUT_US);
        break;

    case FRAME_STAGE_BIT_FALL:
        ESP_LOGE(TAG, "收帧超时：第 %d 位的高电平在 %u µs 内没有结束"
                      "（该位之前的低电平为 %u µs）",
                 diag->bit, (unsigned)DHT11_PHASE_TIMEOUT_US,
                 (unsigned)diag->low_us);
        break;

    case FRAME_STAGE_BIT_WINDOW:
        ESP_LOGE(TAG, "第 %d 位高电平 %u µs 落在非法窗口 (%d, %d) µs 内，"
                      "说明时序已被破坏（该位之前的低电平为 %u µs）",
                 diag->bit, (unsigned)diag->high_us,
                 DHT11_BIT0_MAX_US, DHT11_BIT1_MIN_US,
                 (unsigned)diag->low_us);
        break;

    case FRAME_STAGE_BIT_CONFLICT:
        ESP_LOGE(TAG, "第 %d 位判位法冲突：采样法=%d，宽度法=%d，"
                      "高低比较法=%d（高 %u µs / 低 %u µs）",
                 diag->bit, diag->method_a, diag->method_b, diag->method_c,
                 (unsigned)diag->high_us, (unsigned)diag->low_us);
        break;

    case FRAME_STAGE_OK:
    default:
        /* 只有 read_frame() 返回非 ESP_OK 时才会走到这里，正常不会发生 */
        ESP_LOGE(TAG, "收帧失败，但没有可用的诊断信息");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 真正的读取流程                                                      */
/* ------------------------------------------------------------------ */

/*
 * Phase A + Phase B + 三层次校验。
 *
 * 拆成独立函数的目的很实际：dht11_read() 需要在**任何**返回路径上
 * 清掉 s_busy 标志。把它收拢到 dht11_read() 里"调用 → 清标志"，
 * 就不必在每个 return 前重复一遍清理代码（也就不会漏掉某一条路径）。
 */
static esp_err_t do_read(dht11_reading_t *out)
{
    gpio_dev_t *hw = GPIO_LL_GET_HW(0);
    const uint32_t pin = (uint32_t)s_gpio_num;

    /* ================= Phase A：中断开启，主机起始信号 ================= */
    /*
     * 拉低总线，保持 20ms。
     *
     * 注意 pdMS_TO_TICKS(20) 在 100Hz tick 下等于 2 个 tick，
     * 而 vTaskDelay() 保证的是"至少"，实际可能落在 20~30ms 之间。
     * DHT11 手册要求的是 >= 18ms，所以偏长是安全的；
     * 但如果哪天换成 AM2302（Tbe max = 20ms），这 30ms 就偏长了，
     * 那时应当改用忙等或 RMT 来精确产生起始信号。
     *
     * 为什么不用 esp_rom_delay_us() 忙等来换一个精确的 20ms？
     *   那会让本核白白空转 20ms（虽然中断照常响应，但同优先级任务
     *   要等 tick 才能轮转）。本驱动只支持 DHT11，而 DHT11 对起始
     *   低电平只有下限（>= 18ms）、没有 20ms 这个上限，
     *   所以"精确"在这里换不来任何正确性，只换来 CPU 浪费。
     *   真正需要精确起始脉冲的是 AM2302——等支持 DHT22 时再改。
     *
     * 放在临界区之外是刻意的：这 20ms 占一次读取总时长（约 24ms）的绝大部分，
     * 把它排除在中断屏蔽窗口之外，屏蔽时间就只剩 4ms 左右。
     */
    gpio_ll_set_level(hw, pin, 0);
    vTaskDelay(pdMS_TO_TICKS(DHT11_START_LOW_MS));

    /* ================= Phase B：屏蔽本核中断，收帧 ================= */
    /*
     * 只屏蔽**本核**中断，不碰跨核自旋锁。理由见文件头第三节。
     * 从这里到 portCLEAR_INTERRUPT_MASK_FROM_ISR() 之间，
     * 只能有寄存器访问和空循环，不能有日志、不能有阻塞调用。
     */
    const UBaseType_t saved = portSET_INTERRUPT_MASK_FROM_ISR();

    uint8_t raw[DHT11_BYTE_COUNT];
    uint32_t min_high_us = 0;
    uint32_t max_high_us = 0;
    frame_diag_t diag;
    const esp_err_t frame_ret = read_frame(raw, &min_high_us, &max_high_us, &diag);

    /* 收帧一结束立刻恢复中断。这一步必须在任何日志之前。 */
    portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);

    if (frame_ret != ESP_OK) {
        log_frame_error(&diag);
        return frame_ret;
    }

    /* ============ 第二层防线：校验和 ============ */
    const uint8_t hum_int = raw[0];
    const uint8_t hum_dec = raw[1];
    const uint8_t temp_int = raw[2];
    const uint8_t temp_dec = raw[3];
    const uint8_t checksum = raw[4];

    const uint8_t sum =
        (uint8_t)((hum_int + hum_dec + temp_int + temp_dec) & 0xFF);
    if (sum != checksum) {
        /*
         * 把 5 个原始字节全部按十六进制打出来。
         * 用户拿逻辑分析仪一对比就知道是"位错了"还是"整帧偏了"——
         * 只打印"校验和错误"是没法排查的。
         */
        ESP_LOGE(TAG, "校验和不匹配：原始字节 %02X %02X %02X %02X %02X"
                      "（前四字节之和应为 %02X）",
                 (unsigned)hum_int, (unsigned)hum_dec, (unsigned)temp_int,
                 (unsigned)temp_dec, (unsigned)checksum, (unsigned)sum);
        return ESP_ERR_INVALID_CRC;
    }

    /*
     * 小数位：DHT11 手册写明这两个字节恒为 0。
     *
     * 但市面上克隆件确实会往里塞值，而且它们**参与校验和**——
     * 所以这里不硬性失败，只提醒一句。本驱动只报整数，
     * 因为 DHT11 的分辨率本来就只有 1°C / 1%RH，
     * 就算克隆件塞了小数位，也没有任何可信度可言。
     */
    if (hum_dec != 0 || temp_dec != 0) {
        ESP_LOGW(TAG, "小数位非 0（湿度小数 %u，温度小数 %u），"
                      "可能是兼容型号；本驱动只报告整数部分",
                 (unsigned)hum_dec, (unsigned)temp_dec);
    }

    /* ============ 第三层防线：量程 ============ */
    const int humidity = (int)hum_int;
    const int temperature = (int)temp_int;

    if (humidity < DHT11_HUMIDITY_MIN || humidity > DHT11_HUMIDITY_MAX ||
        temperature < DHT11_TEMPERATURE_MIN || temperature > DHT11_TEMPERATURE_MAX) {
        ESP_LOGE(TAG, "读数超范围：湿度 %d%%RH（合法 %d~%d），"
                      "温度 %d°C（合法 %d~%d）",
                 humidity, DHT11_HUMIDITY_MIN, DHT11_HUMIDITY_MAX,
                 temperature, DHT11_TEMPERATURE_MIN, DHT11_TEMPERATURE_MAX);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * 不处理符号位，这是刻意的。
     *
     * DHT11 手册规定量程是 0~50°C，并且**没有定义任何符号位**。
     * 各家实现对"负温"的处理互相矛盾：
     *   - Adafruit 的 DHT 库对 DHT22 借用温度**小数字节**的 bit7 当符号位，
     *     再配合 -1-f 这种古怪的偏移变换；
     *   - 同一份库里的 DHT11 分支却完全不做符号处理；
     *   - esp-idf-lib 也不做。
     * 既然没有权威依据，硬猜一个只会制造"看起来能跑、实际报错值"的假象，
     * 不如明确不做，并把 0~50 之外的值判为非法（上面那一步已经做了）。
     *
     * 要支持 DHT22（-40~80°C），必须自己补上 bit15 的**符号—数值**解码：
     * 温度是 16 位，最高位为 1 表示负，其余 15 位是绝对值的十分之一摄氏度。
     */
    out->temperature = temperature;
    out->humidity = humidity;

    /*
     * 只在成功时更新"上次成功读取时刻"。
     * 失败的读取不能重置这个时间戳——否则传感器接触不良时，
     * 每次失败都会把间隔重新计时，反而让下一次读取更容易被放行，
     * 变成"越坏越读、越读越坏"。
     */
    s_last_read_us = esp_timer_get_time();

    ESP_LOGI(TAG, "温度 %d°C，湿度 %d%%RH（高电平脉宽 %u~%u µs）",
             out->temperature, out->humidity,
             (unsigned)min_high_us, (unsigned)max_high_us);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t dht11_init(const dht11_config_t *config)
{
    if (config == NULL || config->gpio_num < 0) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gpio_num != DHT11_GPIO_UNINIT) {
        ESP_LOGE(TAG, "已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 开漏模式（GPIO_MODE_INPUT_OUTPUT_OD）是单总线的硬性要求：
     *   写 1 → 引脚高阻，等于"松手"，由外部上拉把线拉到 3V3
     *   写 0 → 引脚拉低，等于"压住"
     * 如果配成推挽（GPIO_MODE_INPUT_OUTPUT），主机输出高电平而
     * 从机同时把它拉低，就是两个输出对打——大电流、发热，会烧引脚。
     *
     * 注意字段名是 pull_up_en / pull_down_en（带下划线）。
     * 另外这里**不能**初始化 hys_ctrl_mode —— 那个成员只在
     * #if SOC_GPIO_SUPPORT_PIN_HYS_FILTER 下才存在，
     * 而 ESP32-S3 没有这个能力，写了会编译不过。
     *
     * 内部上拉只是兜底：ESP32 的内部上拉约 45kΩ，配合总线约 100pF 的
     * 分布电容，上升沿的 RC 时间常数约 4.5µs。而最短的低电平只有
     * 26µs 量级，边沿会明显变圆，采样点容易落进不确定区。
     * 所以**正式接线必须外接 5.1kΩ 上拉**，内部上拉不能替代它。
     */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << config->gpio_num,
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,  /* 开漏：写1=释放总线，写0=拉低 */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        /* 此时 s_gpio_num 还没动，不需要回滚任何状态 */
        ESP_LOGE(TAG, "gpio_config 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 配置完成后立刻把线"松手"一次。
     * 不这么做的话，引脚会保持 gpio_config() 之后的初值（低电平），
     * 传感器会以为主机已经开始通信了，之后第一次 dht11_read() 的
     * 起始信号就变成了"接着上次的低电平继续"，时序对不上。
     * 用 gpio_ll_set_level() 而不是 gpio_set_level()，是为了和收帧路径
     * 保持一致——两者对开漏引脚的行为相同，但前者是内联的寄存器写。
     */
    gpio_ll_set_level(GPIO_LL_GET_HW(0), (uint32_t)config->gpio_num, 1);

    /* 先填状态，最后才置 s_gpio_num —— 保证任何失败路径都不会留下
     * "看起来已初始化、实际没配好"的半成品状态。 */
    s_min_interval_ms = config->min_interval_ms;
    s_last_read_us = 0;   /* 0 = 从未成功读取过，见 dht11_read() 的间隔检查 */
    s_busy = false;
    s_gpio_num = config->gpio_num;

    /* 手册要求两次读取间隔 > 2s。Kconfig 会拦住小于 2000 的配置，
     * 但直接调 API 的调用方绕得过去，所以在运行期再提醒一次。 */
    if (config->min_interval_ms != 0 &&
        config->min_interval_ms < DHT11_MIN_INTERVAL_FLOOR_MS) {
        ESP_LOGW(TAG, "最小读取间隔 %u ms 小于手册要求的 %d ms；"
                      "读得太快时传感器会返回旧值或垃圾数据，且不会报任何错",
                 (unsigned)config->min_interval_ms, DHT11_MIN_INTERVAL_FLOOR_MS);
    }

    ESP_LOGI(TAG, "DHT11 初始化完成：GPIO%d，最小读取间隔 %u ms",
             config->gpio_num, (unsigned)config->min_interval_ms);
    return ESP_OK;
}

esp_err_t dht11_read(dht11_reading_t *out)
{
    if (out == NULL) {
        ESP_LOGE(TAG, "out 为 NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gpio_num == DHT11_GPIO_UNINIT) {
        ESP_LOGE(TAG, "尚未初始化，请先调用 dht11_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------------- 最小间隔检查（不碰总线） ---------------- */
    /*
     * 为什么要有这个检查？
     *
     * DHT11 手册明确警告：两次读取间隔小于约 2 秒时，传感器会**返回上一次
     * 的值或垃圾数据，而不给出任何错误标志**。也就是说，读得太快时
     * 协议层看起来完全正常，校验和也过——你只是拿到了旧数据。
     *
     * 这是最难查的一类故障。所以宁可在入口明确拒绝，
     * 也不要让调用方拿到一个"看起来成功、其实是旧值"的读数。
     *
     * s_last_read_us == 0 表示"从未成功读取过"（初始化时置 0，
     * 而本驱动只可能在 FreeRTOS 起来之后被调用，那时 esp_timer 早已离开 0）。
     * 用哨兵而不是额外的布尔变量，是为了让间隔判断只有一处状态。
     *
     * 日志用 ESP_LOGD 而不是 ESP_LOGE：这是调用方的节奏问题，不是驱动器故障，
     * 按调用频率（可能每秒一次）打 ERROR 会把串口刷满。
     * 需要排查时把日志级别调到 DEBUG 就能看到剩余毫秒数。
     */
    if (s_min_interval_ms > 0 && s_last_read_us != 0) {
        const int64_t elapsed_us = esp_timer_get_time() - s_last_read_us;
        const int64_t need_us = (int64_t)s_min_interval_ms * 1000;
        if (elapsed_us < need_us) {
            /* 向上取整，避免"还剩 0 ms"这种没用的提示 */
            const int64_t remain_ms = (need_us - elapsed_us + 999) / 1000;
            ESP_LOGD(TAG, "距离上次成功读取不足 %u ms，还需等待 %d ms",
                     (unsigned)s_min_interval_ms, (int)remain_ms);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* ---------------- 认领本次读取（只持锁几十纳秒） ---------------- */
    /*
     * 为什么是"忙标志 + 立刻失败"，而不是"拿锁排队 24ms"，
     * 见文件头第四节的完整推导。一句话：跨核自旋锁会把 core 0 上的
     * WiFi 协议栈一起冻住，代价远大于收益。
     */
    portENTER_CRITICAL(&s_read_mux);
    if (s_busy) {
        portEXIT_CRITICAL(&s_read_mux);
        ESP_LOGW(TAG, "另一次 dht11_read() 正在进行，本次直接失败以免搅乱总线");
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    portEXIT_CRITICAL(&s_read_mux);

    const esp_err_t ret = do_read(out);

    /* 无论成功还是失败，都必须释放占用标志，否则驱动会永久卡死 */
    portENTER_CRITICAL(&s_read_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_read_mux);

    return ret;
}
