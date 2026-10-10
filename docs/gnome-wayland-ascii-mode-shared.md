# 为什么 ibus-rime 的 ascii_mode 在所有窗口间总是统一

> 结论：在 **GNOME + Wayland** 会话里，`gnome-shell` 是唯一的 IBus 客户端，所有应用共用
> **同一个** `InputContext`，因而共用**同一个** rime engine 对象 / Rime session。
> Rime 的 `ascii_mode` 本身是 **per-session** 的，但这里从头到尾只有一个 session，
> 所以任何窗口按 Shift 都会让所有窗口一起变。**这不是 Rime 的锅，也不是本仓库工具的锅。**

本文记录的是某个真实环境（见文末「环境与版本」）上的抓包结论，附可复现命令。

---

## 一、现象

- 在任意两个应用窗口（例如 Chrome、kitty）里按 Shift 切中/英，所有窗口的 `ascii_mode` 一起变。
- 总线上除了真正在用的 engine 对象，还存在更早创建的孤儿对象；它们的 `ascii_mode` 可能与当前窗口
  不同（见第五节）。

直觉上应该「只改当前聚焦窗口对应的那个 Rime session」，于是容易怀疑：
- 是不是 Rime/librime 把 `ascii_mode` 做成了全局开关？
- 是不是 IBus 把状态广播给了所有 session？

**都不是。**

---

## 二、结论

1. **Rime 的 `ascii_mode` 是 per-session 的**（每个 engine 对象一份，见第四节）。
2. **但这个环境里只存在一个「真正在用」的 session**：所有应用经由 `gnome-shell` 汇聚到同一个
   `InputContext`（本例是 `InputContext_2`），由同一个 engine 对象（本例是 `Engine/6`）服务。
3. 所以「所有窗口一起变」= 它们本来就是**同一个 session**，而不是多个 session 被同步。
4. 总线上确实还存在别的 engine 对象（本例 `Engine/2`、`Engine/3`），但它们收不到你的按键，
   也收不到焦点变化，是**孤儿对象**，与你的窗口无关。

---

## 三、证据（可复现）

### 1. Rime 状态确实是 per-engine-object 的

只对**一个** engine 对象调用 `PropertyActivate`，其余对象纹丝不动：

```sh
ADDR=$(grep -h '^IBUS_ADDRESS=' ~/.config/ibus/bus/* | head -1 | cut -d= -f2- | cut -d, -f1)

busctl --address="$ADDR" call im.rime.Rime /org/freedesktop/IBus/Engine/2 \
    org.freedesktop.IBus.Engine PropertyActivate su InputMode 0
```

这次翻转只作用于 `Engine/2`。而 `ibus-rime-remote` 只读 id 最大的对象（本例 `Engine/6`），
因此它的输出不受影响——这也正是当前工具只针对一个对象、不去枚举汇总的原因。

单独对某个 engine 对象调用 `ProcessKeyEvent`（模拟按键）也是同样结果：只有那个对象变。

→ **说明 `ascii_mode` 是 per-session 的，「统一」不是 Rime 造成的。**

### 2. 真实按键只送进一个 engine 对象

监听 daemon → engine 的调用：

```sh
ADDR=$(grep -h '^IBUS_ADDRESS=' ~/.config/ibus/bus/* | head -1 | cut -d= -f2- | cut -d, -f1)

dbus-monitor --address "$ADDR" \
  "type='method_call',member='ProcessKeyEvent'" \
  "type='method_call',member='FocusIn'" \
  "type='method_call',member='FocusOut'"
```

> 注意：IBus 的迷你 bus 不支持新式 `org.freedesktop.DBus.Monitoring`，`dbus-monitor` 会自动
> 「Falling back to eavesdropping」，实测可用。

在 Chrome 和 kitty 里各按一次 Shift 的那轮捕获里，`ProcessKeyEvent` 的收件对象：

```
141 次  →  /org/freedesktop/IBus/Engine/6
  0 次  →  Engine/2
  0 次  →  Engine/3
```

所有按键（含 Shift，keyval `65505`）**全部只发往 `Engine/6`**。

### 3. 切换窗口也不切换 engine 对象

在 Chrome ↔ kitty 之间切换焦点时，daemon 发出的 `FocusOut` / `FocusIn`：

```
... sender=:1.1 -> :1.0 path=/org/freedesktop/IBus/InputContext_2 ... member=FocusOut
... path=/org/freedesktop/IBus/Engine/6 ... member=FocusOut
... path=/org/freedesktop/IBus/Engine/6 ... member=FocusIn
... path=/org/freedesktop/IBus/InputContext_2 ... member=FocusIn
... path=/org/freedesktop/IBus/Engine/6 ... member=FocusOut
... path=/org/freedesktop/IBus/Engine/6 ... member=FocusIn
```

两个窗口始终经由同一个 `InputContext_2`、同一个 `Engine/6`。**它们是同一个 session。**

### 4. 唯一的客户端是 gnome-shell

发这些调用的客户端唯一名是 `:1.1`；把 `:1.1` 与进程对应起来：

```sh
# ibus-daemon 的已连接 socket 的对端 inode
ss -xp | grep ibus/dbus-XXXX
# 该对端 inode 出现在 gnome-shell 的 fd 里
ls -l /proc/$(pgrep -x gnome-shell)/fd | grep -oE 'socket:\[[0-9]+\]'
```

本例中 `:1.1` = `gnome-shell`（PID 3009）。也就是说，**所有应用（Chrome / kitty / …）的按键
都由 gnome-shell 这一个进程代为送给同一个 `InputContext`。**

---

## 四、机制：GNOME/Wayland 只有一个全局输入上下文

- 在 Wayland 下，应用通过 **Wayland text-input 协议**把输入交给合成器；GNOME 的实现是
  `gnome-shell` 持有**一个** IBus `InputContext`，替所有应用收发按键。
- 因此 **整个桌面只有一个 IBus 输入上下文** → 只有一个 rime engine 对象 → 只有一个 Rime session。
- 结果：这个会话里的 `ascii_mode` 对**所有窗口**都是同一个值。窗口之间没有任何可区分的
  session，自然也就无法「每个窗口各一份」。

对比：如果每个应用各自直连 IBus（例如 X11/旧式 GTK IM module，或某些 XWayland 应用），
daemon 会为每个应用 **`CreateEngine` 出不同的 engine 对象**，那些对象各有独立 session
（这正是 `Engine/2`、`Engine/3` 的来源）；但你的两个窗口走的是前者，不产生独立 session。

---

## 五、与本仓库工具的关系

`ibus-rime-remote` 在总线名 `im.rime.Rime` 下枚举 `/org/freedesktop/IBus/Engine/<id>`，但只对
**id 最大的那一个**读取和翻转。原因：无法查询哪个对象聚焦（`InputContext.GetEngine` 超时、
`Engine.FocusId` 恒为 `false`），而实测真正收按键的正是最后创建的那个。因此它不会去戳孤儿
对象 `Engine/2`、`Engine/3`，`get` 也总是给出单一的值，不会出现「不一致」。

也就是说，「工具只动一个对象」和「GNOME 只给了一个 session」是两件独立的事，二者叠加才让
"统一"看起来格外明显。搞清根因后可知：**你无法通过改这个工具，在 GNOME/Wayland 下实现
按窗口独立的 `ascii_mode`。**

> 注意：**每个 engine 对象都会独立响应** `PropertyActivate`（见证据 1），只是这里只有一个是
> 「活的」。工具选 id 最大者，是对「聚焦对象」的近似，并不是平台提供的精确对应关系。

---

## 六、影响与限制

- **GNOME/Wayland + ibus-rime**：`ascii_mode` 对全部应用是同一个，无法 per-window。
  这是平台/桌面集成方式决定的，不是 Rime 或本工具能解决的。
- 想要「每个窗口独立」：需要让应用各自拥有独立的 IBus 输入上下文，例如
  - 使用 X11 会话；或
  - 应用走各自直连 IBus 的路径（不经 gnome-shell 的全局上下文）。
- `Engine/2`、`Engine/3` 这类孤儿对象可以安全忽略；不要拿它们的状态代表任何窗口。

---

## 七、复现命令速查

```sh
ADDR=$(grep -h '^IBUS_ADDRESS=' ~/.config/ibus/bus/* | head -1 | cut -d= -f2- | cut -d, -f1)

# 1) 当前聚焦的输入上下文
busctl --address="$ADDR" call org.freedesktop.IBus /org/freedesktop/IBus \
       org.freedesktop.IBus CurrentInputContext

# 2) rime 引擎名下所有 engine 对象
busctl --address="$ADDR" tree im.rime.Rime

# 3) 读实时 ascii_mode（只针对 id 最大的 engine 对象）
./ibus-rime-remote get

# 4) 抓 daemon→engine 的真实按键/焦点路由
dbus-monitor --address "$ADDR" \
  "type='method_call',member='ProcessKeyEvent'" \
  "type='method_call',member='FocusIn'" \
  "type='method_call',member='FocusOut'"
```

---

## 八、环境与版本

- 平台：Linux / Wayland（`XDG_SESSION_TYPE=wayland`），会话中 `ibus-daemon --panel disable`
- `ibus` 1.5.34-2，`ibus-rime` 1.6.1-1（Arch 打包），`glib2` 2.90.1-1
- 观察到的对象：`Engine/2`、`Engine/3`、`Engine/6`；`InputContext_1`、`InputContext_2`
- 当前聚焦：`InputContext_2` → `Engine/6`；客户端 `:1.1` = `gnome-shell`
- 相关源码事实：
  - ibus-rime `rime_engine.c`：`PropertyActivate("InputMode")` →
    `rime_api->set_option(rime_engine->session_id, "ascii_mode", !get_option(...))`；
    每个 engine 对象在 `ibus_rime_engine_init()` 里各自 `create_session()`。
  - librime `gear/ascii_composer.cc`：Shift 切换走 `ctx->set_option("ascii_mode", ...)`；
    `context.cc` 的 options 是 **每 Context（每 session）一份**。
  - librime `switcher.cc` 的 `RestoreSavedOptions()` 只会恢复 `switcher/save_options` 里列出的项；
    默认不含 `ascii_mode`，`user.yaml` 里也没有 `var/option/ascii_mode`，故不存在「全局持久化」。
  - ibus `ibusfactory.c`：engine 对象路径为 `/org/freedesktop/IBus/Engine/%d`，
    `%d` 是引擎进程内自增计数（`++factory->priv->id`），**与 daemon 的 `InputContext_N` 编号无关**。
