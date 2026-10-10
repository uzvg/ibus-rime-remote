# ibus-rime-remote

一个极小的命令行工具：通过 D-Bus 读取 / 设置 Rime 输入法的 `ascii_mode`（中 ↔ 英）。

它**不切换 IBus 引擎**、**不依赖 X11/Wayland**、**不写任何本地缓存**，所有信息都来自实时查询。
专为脚本和编辑器设计：`get` 只往 stdout 打印 `on` / `off`，`set` 成功时静默，所有解释性输出都要显式加 `--verbose`。

在 Neovim 中自动切换输入法状态：[**ibus-rime-remote.nvim**](https://github.com/uzvg/ibus-rime-remote.nvim)。

> [!NOTE]
> **本项目由 LLM 生成。** 代码、文档（包括本 README）均由大语言模型（LLM）生成，请自行审阅后再使用。
> 详见文末「关于本项目」一节。

## 功能

- `get`：读取当前 `ascii_mode`，stdout 输出一行 `on`（西文）或 `off`（中文）。
- `set on|off`：幂等地设置 `ascii_mode`，成功时无输出。
- 只和 Rime **引擎对象自身** 的 `org.freedesktop.IBus.Engine` 接口通信，调用 `PropertyActivate("InputMode", 0)`。
- 单值语义：只操作一个引擎对象（路径 id 最大的那个），不存在 `mixed`。

## 依赖与构建

运行依赖 **GLib / GIO**（D-Bus 支持）：

```sh
# Arch
sudo pacman -S glib2
# Debian / Ubuntu
sudo apt install libglib2.0-dev
# Fedora
sudo dnf install glib2-devel
```

构建：

```sh
make            # 生成 ./ibus-rime-remote
make install    # 安装到 ~/.local/bin/ibus-rime-remote
make uninstall  # 从 ~/.local/bin/ 删除
make clean      # 删除生成的二进制
```

`make install` 默认安装到 `~/.local/bin`，可用 `BINDIR` 覆盖：

```sh
make install BINDIR="$HOME/bin"
```

## 用法

```
ibus-rime-remote {get,set} [value] [选项]
```

### 动作

| 动作 | 说明 | stdout |
| --- | --- | --- |
| `get` | 读取当前 `ascii_mode` | `on` 或 `off`（读不到时为空） |
| `set on` | 设为西文（幂等） | 空（静默即成功） |
| `set off` | 设为中文（幂等） | 空（静默即成功） |

`set` 的目标支持别名（大小写不敏感）：

- **on**：`on`、`ascii`、`en`、`1`、`true`、`西文`、`英`
- **off**：`off`、`zh`、`cn`、`0`、`false`、`中文`、`中`

### 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `--engine-name BUS` | `im.rime.Rime` | Rime 引擎总线名（= 组件 XML 里的 `<name>`） |
| `--prop NAME` | `InputMode` | 要激活的属性名，一般不用改 |
| `--address ADDR` | 自动探测 | 手动指定 IBus 总线地址 |
| `--verbose` | 关闭 | 把额外说明（连了哪个地址、选了哪个对象）打印到 stdout |
| `-h` / `--help` | — | 显示帮助 |

总线地址探测顺序：`--address` → 环境变量 `IBUS_ADDRESS` → `~/.config/ibus/bus/` 下的文件 → `DBUS_SESSION_BUS_ADDRESS`。

### 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 运行失败（连不上总线 / 没有引擎对象 / 读不到状态） |
| `2` | 用法错误（动作或目标不合法） |

**约定：** stdout 只放「取值」（`get`）或 `--verbose` 的额外说明；stderr 只留给真正的执行错误。
因此可以把 `get` 当作一个只吐结果的子进程直接调用（例如 Neovim 的 `vim.system`）。

### 示例

```sh
# 读取当前状态，结果直接就是 on/off
ibus-rime-remote get

# 切成英文 / 中文（幂等，可重复执行）
ibus-rime-remote set on
ibus-rime-remote set off

# 用别名
ibus-rime-remote set en
ibus-rime-remote set 中文

# 排查问题时看详细说明（地址、选中的引擎对象等）
ibus-rime-remote get --verbose

# 显式指定总线地址与组件名
ibus-rime-remote get --address "$IBUS_ADDRESS" --engine-name im.rime.Rime
```

在 shell 里直接当开关用：

```sh
if [ "$(ibus-rime-remote get)" = "on" ]; then
  echo "当前是西文"
else
  echo "当前是中文"
fi
```

## 工作原理（简述）

1. 连接到 IBus 总线，枚举总线名（默认 `im.rime.Rime`）下 `/org/freedesktop/IBus/Engine/<id>` 的对象。
2. 选取路径里 **id 最大** 的那个对象（当前环境下最接近「聚焦对象」的近似）。
3. `set`：先读取实时状态，**仅在目标不同时才** 调用一次 `PropertyActivate("InputMode", 0)`（翻转），因此是幂等的。
4. `get`：IBus 没有 `ascii_mode` 的 getter，改为订阅 `RegisterProperties` 信号、对该对象调用 `FocusIn`，从属性列表里解析当前输入模式（`icon` 为 `abc.png` → 西文，`zh.png` → 中文）。

更深入的机制、踩坑记录与实验证据见 [`docs/gnome-wayland-ascii-mode-shared.md`](docs/gnome-wayland-ascii-mode-shared.md) 以及源码顶部的长注释。

## 已知限制

- **单引擎对象**：每个 `/org/freedesktop/IBus/Engine/<id>` 都有各自独立的 Rime session，`ascii_mode` 是 per-session 的。
  本项目只操作 id 最大的那一个；总线上可能存在的孤儿对象不会被改动。
- 在 GNOME / Wayland 会话中，`gnome-shell` 往往是唯一的 IBus 客户端，所有应用共用同一个输入上下文 / 同一个引擎对象，
  因此「所有窗口一起变」是桌面集成的结果，不是本工具或 Rime 造成的，无法做到按窗口独立。
- 若无法读取状态，会退化为「盲翻一次」（不保证幂等），并在 `--verbose` 下说明。

## 手动等价命令

不装本程序也可以用 `busctl` 完成同样的操作：

```sh
ADDR=$(grep -rh '^IBUS_ADDRESS=' ~/.config/ibus/bus/ | head -1 | cut -d= -f2- | cut -d, -f1)

# 列出所有引擎对象
busctl --address="$ADDR" tree im.rime.Rime

# 对某个引擎对象翻转一次 ascii_mode
busctl --address="$ADDR" call im.rime.Rime /org/freedesktop/IBus/Engine/6 \
    org.freedesktop.IBus.Engine PropertyActivate su InputMode 0
```

## 在 VSCode 中自动切换

配合 [VSCodeVim](https://marketplace.visualstudio.com/items?itemName=vscodevim.vim) 扩展，可在模式切换时自动切换中 / 英：
离开插入模式时切到 `defaultIM`（这里设为 `on`，即西文），进入插入模式时用 `obtainIMCmd` 读回并恢复。

在 VSCode 的 `settings.json` 中加入：

```json
"vim.autoSwitchInputMethod.defaultIM": "on",
"vim.autoSwitchInputMethod.switchIMCmd": "/home/uzvg/.local/bin/ibus-rime-remote set {im}",
"vim.autoSwitchInputMethod.obtainIMCmd": "/home/uzvg/.local/bin/ibus-rime-remote get",
"vim.autoSwitchInputMethod.enable": true,
"workbench.settings.applyToAllProfiles": [
    "vim.autoSwitchInputMethod.enable",
    "vim.autoSwitchInputMethod.obtainIMCmd",
    "vim.autoSwitchInputMethod.switchIMCmd"
],
```

`{im}` 会被 VSCodeVim 替换为目标值，正好对应该工具的 `set`；`obtainIMCmd` 对应 `get`。
`workbench.settings.applyToAllProfiles` 让以上设置应用到所有配置档案（profile）。
把命令路径换成你自己的 `ibus-rime-remote` 所在位置即可。

## 关于本项目

> [!IMPORTANT]
> **本项目由 LLM 生成。**
> 源码（`ibus-rime-remote.c`）、构建脚本、文档（含本 README）均由大语言模型生成，
> 可能包含错误或非最优实现。请在使用前自行审阅与验证。欢迎提交 Issue / PR。

## 许可证

本项目采用 [MIT 许可证](LICENSE)。
