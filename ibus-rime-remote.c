/*
 * ibus-rime-remote.c —— 读取 / 设置 Rime 输入法的 ascii_mode（中↔英）
 * ============================================================================
 *
 * 这是一个**纯 D-Bus**的小命令行工具，只有两个动作：
 *
 *     ibus-rime-remote get          读取当前 ascii_mode
 *     ibus-rime-remote set on|off   幂等地设置 ascii_mode
 *
 * 它不切换 IBus 引擎、不依赖 X11/Wayland，也不写任何本地缓存文件；所有信息都
 * 来自实时查询。
 *
 *
 * 零、输出与退出码（给脚本 / 编辑器用）
 * ----------------------------------------------------------------------------
 * 设计目标：正常时 stdout 只放「取值」，方便直接读取；给人看的额外说明默认不打印，
 * 必须显式加 --verbose，而且和取值一样走 stdout（它不是错误）。stderr 只留给真正的
 * 执行错误。因此可以放心地把它当作一个只吐结果的子进程来调用（例如 Neovim 的 vim.system）。
 *
 *     get            stdout: 一行 "on" 或 "off"；读不到时 stdout 为空
 *     set on|off     stdout: 空（静默即成功）
 *
 *     退出码：0 = 成功，1 = 运行失败（连不上 / 无引擎 / 读不到状态），
 *             2 = 用法错误（动作或目标不合法）
 *
 *     --verbose      额外说明（连了哪个地址、选了哪个对象）→ stdout
 *     出错时的短提示                                → stderr
 *
 * 注意：get / set 都只针对**一个**「聚焦」的引擎对象（见第四节），因此不存在
 * "mixed"。只有那一个对象会被读取和翻转，更早创建的孤儿对象一律不碰。
 *
 *
 * 一、它到底做了什么？
 * ----------------------------------------------------------------------------
 * Rime 引擎（ibus-engine-rime）在 IBus 总线上暴露了一组 D-Bus 对象。本工具向这些
 * 对象的 org.freedesktop.IBus.Engine 接口调用：
 *
 *     PropertyActivate("InputMode", 0)
 *
 * 这等价于在输入法状态栏里点一下「中 / A」：rime 引擎收到后，把它**自己的**
 * Rime session 的 ascii_mode 翻转一次。注意是「翻转（toggle）」，不是「设置成某值」。
 *
 * 因此：
 *   · set on  = 若当前已是「西文」则不动，否则调用一次 PropertyActivate 翻过去；
 *   · set off = 反之。
 *   · get     = 直接读状态，不做任何修改。
 *
 *
 * 二、怎么读状态？（IBus 没有 ascii_mode 的 getter）
 * ----------------------------------------------------------------------------
 * IBus 没提供「取 ascii_mode」的方法。但 rime 引擎在收到 FocusIn 时，会重新发出
 * RegisterProperties 信号，里面带着当前输入模式属性的实时值，而且 FocusIn 本身
 * 不会改变 ascii_mode。于是可以「按需读取」：
 *
 *     AddMatch(RegisterProperties) → 订阅 → 对聚焦的引擎对象调 FocusIn → 解析信号
 *
 * 解析规则（IBusProperty 的字段位置见 IBusProperty 的结构）：
 *     icon = ".../abc.png"  → 西文（ascii_mode = on）
 *     icon = ".../zh.png"   → 中文（ascii_mode = off）
 *     icon 认不出来时，退回看 symbol 是否为 "A"。
 *
 *
 * 三、为什么用「引擎对象」，而不是 daemon 的 InputContext，也不是 Panel？
 * ----------------------------------------------------------------------------
 * 本环境（GNOME / Wayland，ibus-daemon 以 `--panel disable` 启动）实测：
 *   · daemon 的 org.freedesktop.IBus.InputContext.PropertyActivate / ProcessKeyEvent /
 *     GetEngine / FocusIn 会一直超时（daemon 收下调用却不回包、也不转发给引擎）；
 *   · 唯一可靠的通道就是 rime 引擎对象自身。
 * 所以本工具只和引擎对象打交道。
 *
 *
 * 四、重要机制：ascii_mode 是「每个会话一份」
 * ----------------------------------------------------------------------------
 * 每个 /org/freedesktop/IBus/Engine/<id> 对象都拥有自己的 Rime session，
 * ascii_mode 就存在各自的 session 里。对某个对象调用 PropertyActivate，只会改那一个
 * 对象的 ascii_mode，**不会**影响其它对象。
 *
 * 但在 GNOME/Wayland 桌面里，gnome-shell 是唯一的 IBus 客户端，所有应用共用同一个
 * 输入上下文，因而也共用同一个引擎对象 / 同一个 session。结果是：所有窗口的
 * ascii_mode 天生就是同一个，无法做到「按窗口独立」。
 *
 *     —— 这部分结论、证据与复现步骤见：docs/gnome-wayland-ascii-mode-shared.md
 *
 * 总线上除了这个「活的」对象，往往还有更早创建的孤儿对象（例如 Engine/2、Engine/3）。
 * 它们各有自己的 session，但与当前窗口无关；去读/翻它们只会让状态看起来「不一致」。
 *
 * 本工具**只操作其中的一个**：路径里 id 最大的那个。这是当前环境下能做到的最接近
 * 「聚焦对象」的近似——真正的聚焦无法查询（InputContext.GetEngine 超时，
 * Engine.FocusId 恒为 false），而实测真正收按键的正是最后创建的那个对象。
 * 结论与证据：docs/gnome-wayland-ascii-mode-shared.md。
 *
 *
 * 五、GOTCHA（都是踩过的坑）
 * ----------------------------------------------------------------------------
 *   1. g_dbus_connection_new_for_address_sync() **不会**自动发送 D-Bus 的 Hello。
 *      标准 dbus-daemon 会因此拒绝后续所有调用，所以连接后必须手动调用 Hello。
 *   2. IBus 自带的是「迷你 bus」，GDBus 的 signal_subscribe 自带的 AddMatch 在它上面
 *      **不生效**（订阅了却收不到信号）。必须自己再发一条 org.freedesktop.DBus.AddMatch。
 *   3. PropertyActivate 只要调用成功就返回成功，但它**不是**「状态已改变」的信号：
 *      调用未聚焦的对象同样返回成功。真正的状态要以 get 读到的为准。
 *   4. IBus 迷你 bus 没有实现 ListActivatableNames；`busctl list` 会误报
 *      EBADR "Invalid request descriptor"。要列对象请用 `busctl tree im.rime.Rime`。
 *   5. 引擎的总线名 = /usr/share/ibus/component/rime.xml 里 <component><name> 的值
 *      （默认 im.rime.Rime）。组件名不对时用 --engine-name 指定。
 *   6. 引擎对象路径里的数字是**引擎进程内的自增计数器**（ibusfactory.c 里的
 *      `++factory->priv->id`），和 daemon 的 InputContext_<N> 编号毫无关系。
 *   7. 总线地址里可能带 guid。GDBus 不校验 guid，带不带都能连，所以本工具**不做**
 *      guid 处理（只有 sd-bus/busctl 才校验，残留的过期 guid 会让它报
 *      "Operation not permitted"）。
 *   8. 想抓 daemon→engine 的真实按键/焦点流向，可以监听总线：
 *        dbus-monitor --address "$ADDR" \
 *          "type='method_call',member='ProcessKeyEvent'" \
 *          "type='method_call',member='FocusIn'" \
 *          "type='method_call',member='FocusOut'"
 *      （IBus 迷你 bus 不支持新式 Monitoring，dbus-monitor 会自动回退到 eavesdropping，可用。）
 *
 *
 * 六、手动等价命令（不用本程序也能做）
 * ----------------------------------------------------------------------------
 *     ADDR=$(grep -rh '^IBUS_ADDRESS=' ~/.config/ibus/bus/ | head -1 | cut -d= -f2- | cut -d, -f1)
 *
 *     # 列出所有引擎对象
 *     busctl --address="$ADDR" tree im.rime.Rime
 *
 *     # 对某个引擎对象翻转一次 ascii_mode
 *     busctl --address="$ADDR" call im.rime.Rime /org/freedesktop/IBus/Engine/6 \
 *         org.freedesktop.IBus.Engine PropertyActivate su InputMode 0
 *
 *
 * 七、构建与依赖
 * ----------------------------------------------------------------------------
 *     make            # 依赖 gcc/clang + pkg-config + glib2（Arch: pacman -S glib2）
 *     make clean
 */

#include <gio/gio.h> /* GDBus、GVariant、GMainLoop 等（已包含 glib.h） */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <locale.h> /* setlocale：见 main 开头 */

/* ============================================================================
 * 常量：D-Bus 名字 / 接口 / 对象路径
 * ==========================================================================*/

/* 标准 D-Bus 守护进程本身（用于 Hello、AddMatch 等） */
#define DBUS_BUS_NAME    "org.freedesktop.DBus"
#define DBUS_OBJECT_PATH "/org/freedesktop/DBus"
#define DBUS_IFACE       "org.freedesktop.DBus"

/* IBus daemon */
#define IBUS_BUS_NAME    "org.freedesktop.IBus"
#define IBUS_OBJECT_PATH "/org/freedesktop/IBus"

/* rime 引擎 */
#define ENGINE_IFACE            "org.freedesktop.IBus.Engine"
#define INTROSPECT_IFACE        "org.freedesktop.DBus.Introspectable"
#define ENGINE_ROOT_PATH        "/org/freedesktop/IBus/Engine" /* 子节点是 /<id> */
#define ENGINE_BUS_NAME_DEFAULT "im.rime.Rime"                 /* 组件名 = 总线名 */
#define PROP_INPUT_MODE         "InputMode"                    /* ascii_mode 的属性名 */

/* 各种同步调用的超时（毫秒） */
#define PROBE_TIMEOUT_MS 500  /* 探测 / Hello 这类小调用 */
#define CALL_TIMEOUT_MS  2000 /* 正常的业务调用 */
#define READ_WAIT_MS     1200 /* 等 RegisterProperties 信号的最长时间 */

/* 退出码：0 成功，1 运行失败，2 用法错误（见文件头「输出与退出码」）。 */
#define EXIT_OK    0
#define EXIT_FAIL  1
#define EXIT_USAGE 2

/* ============================================================================
 * 小工具
 * ==========================================================================*/

/* 是否打印额外说明（由 --verbose 打开）。默认闭嘴，stdout 只留取值。 */
static gboolean g_verbose = FALSE;

/*
 * 打印一条额外说明到 **stdout**；只有 --verbose 时才输出。
 * 说明不是错误，所以走 stdout；stderr 只留给真正的执行错误（g_printerr）。
 */
static void verbosef(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[verbose] ", stdout);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}


/* ============================================================================
 * D-Bus 基础调用
 * ==========================================================================*/

/*
 * 对总线发一个同步方法调用，等待并返回回复。
 *   · reply_type 传 NULL 表示「不校验返回类型」。
 *   · 失败返回 NULL，并把错误写进 *error（调用方负责 g_error_free）。
 */
static GVariant *call_method(GDBusConnection *conn,
                             const char *dest, const char *path,
                             const char *iface, const char *method,
                             GVariant *params, int timeout_ms, GError **error)
{
    return g_dbus_connection_call_sync(conn, dest, path, iface, method, params,
                                       NULL, G_DBUS_CALL_FLAGS_NONE,
                                       timeout_ms, NULL, error);
}

/*
 * 发送 D-Bus 的 Hello 握手（见文件头 GOTCHA 1）。
 * g_dbus_connection_new_for_address_sync 不会自动发，必须手动补。
 * 失败也没什么可做的，忽略即可。
 */
static void send_hello(GDBusConnection *conn)
{
    GError *error = NULL;
    GVariant *reply = call_method(conn, DBUS_BUS_NAME, DBUS_OBJECT_PATH, DBUS_IFACE,
                                  "Hello", NULL, PROBE_TIMEOUT_MS, &error);
    if (reply)
        g_variant_unref(reply);
    g_clear_error(&error);
}

/*
 * 判断这个连接背后是不是 IBus daemon。
 * 首选 GetNameOwner；某些 mini-bus 没实现它，就退回用真实的业务方法
 * CurrentInputContext 探测（见文件头 GOTCHA 4）。
 */
static gboolean probe_is_ibus(GDBusConnection *conn)
{
    GError *error = NULL;
    GVariant *reply;

    reply = call_method(conn, DBUS_BUS_NAME, DBUS_OBJECT_PATH, DBUS_IFACE,
                        "GetNameOwner", g_variant_new("(s)", IBUS_BUS_NAME),
                        PROBE_TIMEOUT_MS, &error);
    if (reply) {
        g_variant_unref(reply);
        return TRUE;
    }
    g_clear_error(&error);

    reply = call_method(conn, IBUS_BUS_NAME, IBUS_OBJECT_PATH, IBUS_BUS_NAME,
                        "CurrentInputContext", NULL, PROBE_TIMEOUT_MS, &error);
    if (reply) {
        g_variant_unref(reply);
        return TRUE;
    }
    g_clear_error(&error);
    return FALSE;
}

/* ============================================================================
 * 找出 IBus 的总线地址并连接
 * ==========================================================================*/

/* 只接受看起来像 D-Bus 地址的字符串，挡掉注释行、文件名等。 */
static gboolean is_dbus_address(const char *s)
{
    return g_str_has_prefix(s, "unix:") ||
           g_str_has_prefix(s, "tcp:") ||
           g_str_has_prefix(s, "unixexec:") ||
           g_str_has_prefix(s, "nonce-tcp:");
}

/* 地址列表里是否已有这一条（用于去重）。 */
static gboolean address_list_contains(GPtrArray *list, const char *addr)
{
    for (guint i = 0; i < list->len; i++)
        if (g_strcmp0(g_ptr_array_index(list, i), addr) == 0)
            return TRUE;
    return FALSE;
}

/* 把候选地址加入列表：去空白、校验、去重。 */
static void add_address(GPtrArray *list, const char *raw)
{
    if (!raw)
        return;
    char *addr = g_strstrip(g_strdup(raw)); /* g_strstrip 原地去首尾空白 */
    if (is_dbus_address(addr) && !address_list_contains(list, addr))
        g_ptr_array_add(list, addr);
    else
        g_free(addr);
}

/*
 * 从 ~/.config/ibus/bus/ 下的一个文件里读地址。
 * 文件里通常是 `IBUS_ADDRESS=unix:path=...`，也可能直接就是一行地址。
 */
static char *address_from_bus_file(const char *file_path)
{
    char *content = NULL;
    if (!g_file_get_contents(file_path, &content, NULL, NULL))
        return NULL;

    char *result = NULL;
    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        if (*line == '\0' || *line == '#')
            continue;
        if (g_str_has_prefix(line, "IBUS_ADDRESS=")) {
            result = g_strdup(line + strlen("IBUS_ADDRESS="));
            break;
        }
        if (is_dbus_address(line)) {
            result = g_strdup(line);
            break;
        }
    }
    g_strfreev(lines);
    g_free(content);
    return result;
}

/*
 * 收集所有「可能」的 IBus 总线地址，按优先级排列：
 *   1. --address 指定
 *   2. 环境变量 IBUS_ADDRESS
 *   3. ~/.config/ibus/bus/ 下的文件
 *   4. 环境变量 DBUS_SESSION_BUS_ADDRESS
 * 返回 GPtrArray<gchar*>，用完用 g_ptr_array_unref 释放。
 */
static GPtrArray *candidate_addresses(const char *override)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    const char *value;

    if (override)
        add_address(list, override);

    if ((value = g_getenv("IBUS_ADDRESS")) != NULL)
        add_address(list, value);

    char *busdir = g_build_filename(g_get_home_dir(), ".config", "ibus", "bus", NULL);
    GDir *dir = g_dir_open(busdir, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.')
                continue;
            char *path = g_build_filename(busdir, name, NULL);
            if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                char *addr = address_from_bus_file(path);
                if (addr) {
                    add_address(list, addr);
                    g_free(addr);
                }
            }
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_free(busdir);

    if ((value = g_getenv("DBUS_SESSION_BUS_ADDRESS")) != NULL)
        add_address(list, value);

    return list;
}

/*
 * 依次尝试候选地址，返回第一个能连上、且确实是 IBus 的连接。
 * 全部失败则打印诊断信息并退出。
 */
static GDBusConnection *connect_ibus(const char *override)
{
    GPtrArray *candidates = candidate_addresses(override);
    GString *errors = g_string_new(NULL);
    GDBusConnection *result = NULL;

    for (guint i = 0; i < candidates->len && result == NULL; i++) {
        const char *address = g_ptr_array_index(candidates, i);

        GError *error = NULL;
        GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
            address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
        if (!conn) {
            g_string_append_printf(errors, "  %s\n      -> %s\n", address, error->message);
            g_clear_error(&error);
            continue;
        }

        send_hello(conn);
        if (probe_is_ibus(conn)) {
            verbosef("已连接 IBus 总线：%s", address);
            result = conn;
        } else {
            g_string_append_printf(errors, "  %s\n      -> 该总线上没有 org.freedesktop.IBus\n",
                                   address);
            g_object_unref(conn);
        }
    }

    if (!result) {
        g_printerr("错误：找不到 IBus daemon\n");
        verbosef("试过这些地址：\n%s\n"
                 "排查：\n"
                 "  1. ps -eo pid,args | grep -i '[i]bus'          # daemon 真的在跑吗\n"
                 "  2. grep -H IBUS_ADDRESS ~/.config/ibus/bus/*   # 文件里的地址还新吗\n"
                 "  3. 手动指定（去掉 guid 更稳）：\n"
                 "       --address \"$(grep -h '^IBUS_ADDRESS=' ~/.config/ibus/bus/* "
                 "| head -1 | cut -d= -f2- | cut -d, -f1)\"\n\n"
                 "注意：不要用 `busctl list` 判断 IBus 私有总线是否可用——迷你 bus 没有实现\n"
                 "ListActivatableNames，会误报 EBADR \"Invalid request descriptor\"。",
                 candidates->len > 0 ? errors->str : "  （没有候选地址）");
    }

    g_string_free(errors, TRUE);
    g_ptr_array_unref(candidates);
    if (!result)
        exit(EXIT_FAILURE);
    return result;
}

/* ============================================================================
 * 枚举 rime 引擎对象
 * ==========================================================================*/

/* g_array_sort 用的整数比较函数 */
static gint cmp_gint64(gconstpointer a, gconstpointer b)
{
    gint64 x = *(const gint64 *)a;
    gint64 y = *(const gint64 *)b;
    return (x > y) - (x < y);
}

/*
 * Introspect 父节点 /org/freedesktop/IBus/Engine，子节点名就是数字 id。
 * 返回 GPtrArray<gchar*>（路径），可能为空；用完用 g_ptr_array_unref 释放。
 */
static GPtrArray *engine_paths(GDBusConnection *conn, const char *engine_name)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    GError *error = NULL;
    GVariant *reply = call_method(conn, engine_name, ENGINE_ROOT_PATH, INTROSPECT_IFACE,
                                  "Introspect", NULL, CALL_TIMEOUT_MS, &error);
    if (!reply) {
        verbosef("Introspect 失败：%s", error ? error->message : "未知错误");
        g_clear_error(&error);
        return paths;
    }

    const char *xml = NULL;
    g_variant_get(reply, "(&s)", &xml);

    /* 从 XML 里抓出所有 <node name="数字">，去重后按数值排序 */
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (const char *p = xml; (p = strstr(p, "<node name=\"")) != NULL;) {
        p += strlen("<node name=\"");
        if (!g_ascii_isdigit(*p))
            continue;
        char *end = NULL;
        gint64 id = g_ascii_strtoll(p, &end, 10);
        if (end && *end == '"') {
            gboolean duplicate = FALSE;
            for (guint i = 0; i < ids->len; i++)
                if (g_array_index(ids, gint64, i) == id) {
                    duplicate = TRUE;
                    break;
                }
            if (!duplicate)
                g_array_append_val(ids, id);
        }
        if (end)
            p = end;
    }
    g_variant_unref(reply);

    g_array_sort(ids, cmp_gint64);
    for (guint i = 0; i < ids->len; i++) {
        gint64 id = g_array_index(ids, gint64, i);
        g_ptr_array_add(paths, g_strdup_printf("%s/%" G_GINT64_FORMAT, ENGINE_ROOT_PATH, id));
    }
    g_array_free(ids, TRUE);

    verbosef("发现 %u 个引擎对象", paths->len);
    return paths;
}

/* 触发表情：对某个引擎对象翻转一次 InputMode（见文件头 GOTCHA 3）。 */
static void activate_input_mode(GDBusConnection *conn, const char *engine_name,
                                const char *prop, const char *engine_path)
{
    GError *error = NULL;
    GVariant *reply = call_method(conn, engine_name, engine_path, ENGINE_IFACE,
                                  "PropertyActivate", g_variant_new("(su)", prop, (guint32)0),
                                  CALL_TIMEOUT_MS, &error);
    if (reply)
        g_variant_unref(reply);
    if (error) {
        verbosef("对 %s 调用 PropertyActivate 失败：%s", engine_path, error->message);
        g_error_free(error);
    }
}

/* ============================================================================
 * 读取实时 ascii_mode
 *
 * 流程：AddMatch → 订阅 RegisterProperties → 对每个引擎对象调 FocusIn → 解析信号。
 * 见文件头第二节。
 * ==========================================================================*/

/* 订阅用：把「收到的信号」和「主循环」串起来的小上下文（只关心单个对象） */
typedef struct {
    GMainLoop *loop;
    const char *target; /* 只接受这个引擎对象路径发来的信号 */
    int state;          /* 0/1；-1 表示还没解析到 */
    guint timeout_id;   /* 超时源 id；0 表示已触发/已移除 */
} read_state_ctx;

/* 从 v(IBusText) 里取出文本（IBusText: (schema, attrs, text)） */
static char *ibustext_dup(GVariant *value)
{
    GVariant *text_object = g_variant_get_variant(value); /* v -> IBusText */
    GVariant *text = g_variant_get_child_value(text_object, 2);
    const char *s = g_variant_get_string(text, NULL);
    char *result = s ? g_strdup(s) : NULL;
    g_variant_unref(text);
    g_variant_unref(text_object);
    return result;
}

/*
 * 由 icon（其次 symbol）判断中/英：
 *   abc.png -> 1（西文），zh.png -> 0（中文），认不出来 -> -1。
 */
static int ascii_from_icon(const char *icon, const char *symbol)
{
    const char *base = icon ? strrchr(icon, '/') : NULL;
    base = base ? base + 1 : icon;
    if (base && strcmp(base, "abc.png") == 0)
        return 1;
    if (base && strcmp(base, "zh.png") == 0)
        return 0;
    if (symbol && strcmp(symbol, "A") == 0)
        return 1;
    return -1;
}

/*
 * 从 RegisterProperties 信号的参数里找出 InputMode 属性，返回 0/1/-1。
 *
 * 信号参数是 (v)，解开后是 IBusPropList: (schema, attrs, av props)。
 * 每个 prop 再解开是 IBusProperty: (…, key(s), …, icon(s), …)，
 * 其中 key 在字段 2、icon 在字段 5、symbol(v IBusText) 在字段 11。
 */
static int parse_inputmode_ascii(GVariant *signal_params)
{
    GVariant *value = g_variant_get_child_value(signal_params, 0); /* v */
    GVariant *prop_list = g_variant_get_variant(value);            /* IBusPropList */
    GVariant *props = g_variant_get_child_value(prop_list, 2);     /* av */

    int result = -1;
    GVariantIter iter;
    g_variant_iter_init(&iter, props);
    GVariant *item;
    while ((item = g_variant_iter_next_value(&iter)) != NULL) {
        GVariant *prop = g_variant_get_variant(item); /* IBusProperty */
        GVariant *keyv = g_variant_get_child_value(prop, 2);
        const char *key = g_variant_get_string(keyv, NULL);

        if (key && strcmp(key, "InputMode") == 0) {
            GVariant *iconv = g_variant_get_child_value(prop, 5);
            const char *icon = g_variant_get_string(iconv, NULL);
            GVariant *symv = g_variant_get_child_value(prop, 11);
            char *symbol = ibustext_dup(symv);
            result = ascii_from_icon(icon, symbol);
            g_free(symbol);
            g_variant_unref(symv);
            g_variant_unref(iconv);
        }

        g_variant_unref(keyv);
        g_variant_unref(prop);
        g_variant_unref(item);
        if (result >= 0)
            break;
    }

    g_variant_unref(props);
    g_variant_unref(prop_list);
    g_variant_unref(value);
    return result;
}

/* 收到 RegisterProperties：只认目标引擎对象发来的那条，解析后结束循环。 */
static void on_register_properties(GDBusConnection *conn, const char *sender,
                                   const char *path, const char *iface,
                                   const char *signal, GVariant *params, gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)iface;
    (void)signal;

    read_state_ctx *ctx = user_data;
    if (strcmp(path, ctx->target) != 0)
        return; /* 别的引擎对象发来的，忽略 */
    if (ctx->state >= 0)
        return; /* 已经拿到 */

    int ascii = parse_inputmode_ascii(params);
    if (ascii < 0)
        return; /* 没有 InputMode 属性，忽略 */

    ctx->state = ascii;
    verbosef("%s -> ascii_mode=%s", path, ascii ? "on" : "off");
    g_main_loop_quit(ctx->loop);
}

/* 等待信号超时：结束主循环。 */
static gboolean on_read_timeout(gpointer user_data)
{
    read_state_ctx *ctx = user_data;
    ctx->timeout_id = 0;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

/* 手动发一条 AddMatch（见文件头 GOTCHA 2）。 */
static void dbus_add_match(GDBusConnection *conn, const char *rule)
{
    GError *error = NULL;
    GVariant *reply = call_method(conn, DBUS_BUS_NAME, DBUS_OBJECT_PATH, DBUS_IFACE,
                                  "AddMatch", g_variant_new("(s)", rule), CALL_TIMEOUT_MS, &error);
    if (reply)
        g_variant_unref(reply);
    if (error) {
        verbosef("AddMatch 失败：%s", error->message);
        g_error_free(error);
    }
}

/*
 * 选出「聚焦」的引擎对象路径。
 *
 * 本环境无法真正查询哪个对象聚焦：InputContext.GetEngine 会超时，Engine.FocusId 也
 * 恒为 false。因此采用一个近似——**取 id 最大的对象**（engine_paths 已按 id 升序）。
 * 依据见 docs/gnome-wayland-ascii-mode-shared.md：真正收按键的是最后创建的那个，
 * 更早创建的孤儿对象只会污染读数。
 *
 * 返回 g_strdup 出来的路径；没有引擎对象时返回 NULL（调用方 g_free）。
 */
static char *focused_engine_path(GDBusConnection *conn, const char *engine_name)
{
    GPtrArray *paths = engine_paths(conn, engine_name);
    char *result = NULL;
    if (paths->len > 0) {
        result = g_strdup(g_ptr_array_index(paths, paths->len - 1));
        verbosef("选取聚焦引擎对象 %s（共 %u 个，取 id 最大者）", result, paths->len);
    }
    g_ptr_array_unref(paths);
    return result;
}

/*
 * 读取某一个引擎对象的实时 ascii_mode：AddMatch → 订阅 RegisterProperties →
 * 对该对象调 FocusIn → 解析信号。返回 0/1，读不到则 -1。
 */
static int read_engine_state(GDBusConnection *conn, const char *engine_name, const char *path)
{
    dbus_add_match(conn, "type='signal',interface='" ENGINE_IFACE "',member='RegisterProperties'");

    read_state_ctx ctx = {
        .loop = g_main_loop_new(NULL, FALSE),
        .target = path,
        .state = -1,
        .timeout_id = 0,
    };

    guint sub = g_dbus_connection_signal_subscribe(
        conn, NULL, ENGINE_IFACE, "RegisterProperties", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_register_properties, &ctx, NULL);

    /* FocusIn 触发它重发 RegisterProperties */
    GError *error = NULL;
    GVariant *reply = call_method(conn, engine_name, path, ENGINE_IFACE,
                                  "FocusIn", NULL, CALL_TIMEOUT_MS, &error);
    if (reply)
        g_variant_unref(reply);
    if (error) {
        verbosef("对 %s 调用 FocusIn 失败：%s", path, error->message);
        g_error_free(error);
    }

    ctx.timeout_id = g_timeout_add(READ_WAIT_MS, on_read_timeout, &ctx);
    g_main_loop_run(ctx.loop);
    if (ctx.timeout_id)
        g_source_remove(ctx.timeout_id);

    g_dbus_connection_signal_unsubscribe(conn, sub);
    g_main_loop_unref(ctx.loop);
    return ctx.state;
}

/* ============================================================================
 * 命令行动作：get / set
 * ==========================================================================*/

/*
 * get：读取并打印聚焦引擎对象的 ascii_mode。
 *   · stdout 只输出一行 "on" 或 "off"，没有任何前缀、颜色、中文或路径。
 *   · 没有引擎对象、或读不到状态时，stdout 留空并返回 EXIT_FAIL。
 */
static int do_get(GDBusConnection *conn, const char *engine_name)
{
    char *path = focused_engine_path(conn, engine_name);
    if (!path) {
        verbosef("总线上没有引擎对象");
        return EXIT_FAIL;
    }

    int state = read_engine_state(conn, engine_name, path);
    g_free(path);
    if (state < 0) {
        verbosef("读不到该引擎对象的 ascii_mode");
        return EXIT_FAIL;
    }

    g_print("%s\n", state ? "on" : "off");
    return EXIT_OK;
}

/* 解析 set 的目标值，成功返回 TRUE。 */
static gboolean parse_target(const char *value, gboolean *out)
{
    static const char *const ON[] = {"on", "ascii", "en", "1", "true", "西文", "英", NULL};
    static const char *const OFF[] = {"off", "zh", "cn", "0", "false", "中文", "中", NULL};

    char *lower = g_ascii_strdown(value, -1);
    gboolean ok = FALSE;

    for (int i = 0; ON[i]; i++)
        if (strcmp(lower, ON[i]) == 0) {
            *out = TRUE;
            ok = TRUE;
            break;
        }
    if (!ok)
        for (int i = 0; OFF[i]; i++)
            if (strcmp(lower, OFF[i]) == 0) {
                *out = FALSE;
                ok = TRUE;
                break;
            }

    g_free(lower);
    return ok;
}

/* 没找到任何引擎对象：默认一句短错误，排查步骤留给 --verbose。 */
static void die_no_engine(const char *engine_name)
{
    g_printerr("错误：总线名 %s 上没有 rime 引擎对象\n", engine_name);
    verbosef("排查：\n"
             "  1. rime 引擎在跑吗：ps -eo pid,args | grep -F ibus-engine-rime\n"
             "  2. 看对象：busctl --address=\"$(grep -h '^IBUS_ADDRESS=' ~/.config/ibus/bus/* "
             "| head -1 | cut -d= -f2- | cut -d, -f1)\" tree %s\n"
             "  3. 组件名不对？看 /usr/share/ibus/component/rime.xml 的 <component><name>，"
             "用 --engine-name 指定",
             engine_name);
    exit(EXIT_FAIL);
}

/*
 * set：先读聚焦引擎对象的实时状态，仅在它尚未处于目标状态时才翻转，因此是幂等的。
 * 成功时 stdout 为空（静默即成功）；做了什么只在 --verbose 时写到 stderr。
 * 读不到状态时退回「盲翻一次」（不保证幂等），同样只在 --verbose 下说明。
 */
static int do_set(GDBusConnection *conn, const char *engine_name,
                  const char *prop, const char *value)
{
    gboolean target;
    if (!parse_target(value, &target)) {
        g_printerr("无法识别的目标状态：%s\n", value);
        return EXIT_USAGE;
    }

    char *path = focused_engine_path(conn, engine_name);
    if (!path)
        die_no_engine(engine_name);

    int state = read_engine_state(conn, engine_name, path);
    if (state < 0) {
        /* 降级路径：读不到状态，只能盲翻一次，不保证幂等 */
        activate_input_mode(conn, engine_name, prop, path);
        verbosef("读不到实时状态，已盲翻 %s（不保证幂等）", path);
    } else if ((state != 0) != target) {
        activate_input_mode(conn, engine_name, prop, path);
        verbosef("已翻转为 %s（%s）", target ? "on" : "off", path);
    } else {
        verbosef("已是目标状态（%s），未操作", path);
    }

    g_free(path);
    return EXIT_OK;
}

/* ============================================================================
 * 入口
 * ==========================================================================*/

int main(int argc, char **argv)
{
    /* 启用用户 locale。否则进程处于 "C" locale，而 GLib 把 --help 输出到终端时
     * 会按 locale 字符集转换 UTF-8 文本；在 "C" locale 下中文会全变成 '?'。 */
    setlocale(LC_ALL, "");

    char *opt_prop = NULL;
    char *opt_engine = NULL;
    char *opt_address = NULL;
    gboolean opt_verbose = FALSE;
    char **rest = NULL; /* 非选项参数：action [value] */

    GOptionEntry entries[] = {
        {"prop", 0, 0, G_OPTION_ARG_STRING, &opt_prop,
         "属性名（ibus-rime 为 InputMode，一般不用改）", "NAME"},
        {"engine-name", 0, 0, G_OPTION_ARG_STRING, &opt_engine,
         "rime 引擎总线名（默认 im.rime.Rime）", "BUS"},
        {"address", 0, 0, G_OPTION_ARG_STRING, &opt_address,
         "手动指定 IBus 总线地址", "ADDR"},
        {"verbose", 0, 0, G_OPTION_ARG_NONE, &opt_verbose,
         "把额外说明打印到 stdout（默认完全安静）", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &rest,
         "ACTION [VALUE]", "get 或 set on|off"},
        {NULL}
    };

    GOptionContext *context = g_option_context_new("- 读取 / 设置 Rime 的 ascii_mode");
    g_option_context_set_summary(context,
        "直接调用 rime 引擎对象的 PropertyActivate(\"InputMode\")，"
        "不切换 IBus 引擎。\n"
        "  get          stdout 输出 on 或 off（读不到则空）\n"
        "  set on|off   幂等设置，成功时无输出\n"
        "set 的目标也可写 en/zh、ascii/中文。\n"
        "退出码：0 成功，1 运行失败，2 用法错误；额外说明加 --verbose。");
    g_option_context_add_main_entries(context, entries, NULL);

    GError *error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("参数解析失败：%s\n", error->message);
        g_error_free(error);
        g_option_context_free(context);
        g_strfreev(rest);
        g_free(opt_prop);
        g_free(opt_engine);
        g_free(opt_address);
        return EXIT_USAGE;
    }
    g_option_context_free(context);

    g_verbose = opt_verbose;

    const char *action = (rest && rest[0]) ? rest[0] : NULL;
    const char *value = (rest && rest[1]) ? rest[1] : NULL;
    const char *prop = opt_prop ? opt_prop : PROP_INPUT_MODE;
    const char *engine_name = opt_engine ? opt_engine : ENGINE_BUS_NAME_DEFAULT;

    int status = EXIT_OK;
    if (!action) {
        g_printerr("缺少动作。用法：ibus-rime-remote {get,set} [value] [选项]\n"
                   "试试：ibus-rime-remote --help\n");
        status = EXIT_USAGE;
        goto out;
    }
    if (strcmp(action, "get") != 0 && strcmp(action, "set") != 0) {
        g_printerr("无效的动作：%s（可选 get / set）\n", action);
        status = EXIT_USAGE;
        goto out;
    }
    if (rest && g_strv_length(rest) > 2) {
        g_printerr("多余的参数：%s\n", rest[2]);
        status = EXIT_USAGE;
        goto out;
    }
    if (strcmp(action, "set") == 0 && (!value || !*value)) {
        g_printerr("set 需要一个目标：on/off（或 en/zh、ascii/中文）\n");
        status = EXIT_USAGE;
        goto out;
    }

    {
        GDBusConnection *conn = connect_ibus(opt_address);
        status = (strcmp(action, "get") == 0)
                     ? do_get(conn, engine_name)
                     : do_set(conn, engine_name, prop, value);
        g_object_unref(conn);
    }

out:
    g_strfreev(rest);
    g_free(opt_prop);
    g_free(opt_engine);
    g_free(opt_address);
    return status;
}
