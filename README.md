# 7K2 AI Mod — 七个王国 2 HD 的 AI 改造与研究

> **Seven Kingdoms II: The Fryhtan Wars — AI behaviour overhaul & reverse-engineering notes**
>
> 给 **《七王国 2 HD》（Steam / GOG 版）** 做的 AI 增强补丁 + 一份完整的**引擎逆向地图**。
> 不改游戏本体、不替换 `7K2.exe`，全部改动都在一个插件 DLL 里，随时可卸载。

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

---

## 这是什么

七王国 2 是 1999 年 Enlight 出的 RTS。它的 AI 有几个**结构性的硬伤**（下面细说），
玩家普遍的感受是：**魔族不会发展、叛军像傻子、清剿叛军反而把自己搞死**。

这个项目做的事：

| # | 改动 | 玩家能感受到的效果 |
|---|---|---|
| 1 | **魔族会开矿** | 有城镇的魔族（牛头人除外）终于能建矿场 |
| 2 | **魔族扩军 / 造堡垒** | 有城镇没兵营的魔族会造堡垒（⇒ 有将军位 ⇒ 能带兵 ⇒ 能发展） |
| 3 | **叛乱者不再无脑送死** | 弱队不攻城、会挑软的打、能自己建中立新城 |
| 4 | **叛军血量 ×2** | 更难清剿，但也更耐打 |
| 5 | **清剿叛军不再自毁** | 杀叛军声誉只掉 0.3/人（原版 -1），**同民族忠诚完全不掉** |

外加一个**显示修复**：游戏内日期原本会丢字符（`月 7, 100`），现在正常显示（`1002年4月12日` / `1002/5/29`）。

**这些都跑在真实游戏里、由玩家实测确认过**（不是"理论上可行"）。

---

## 为什么值得开源

因为这个项目的**副产品**可能比功能本身更有价值：

- **一份经过实测验证的 7K2 引擎地址表** —— 函数地址、结构体偏移、数组布局，全部有出处、有验证
- **一套"从二进制反推语义"的方法论** —— 包括哪些方法**有效**、哪些**浪费时间**
- **一份诚实的死路清单** —— 我们走过的坑、判过的错，别人可以直接跳过

如果你也在折腾 7K2（或者同引擎的 7KAA），这些能省你**几周**。

---

## 技术亮点（给想过深水区的朋友）

### 三个引擎级结构性缺陷

**① 魔族永久锁死在经济起步前**

```c
// Unit::ai_build_firm() 第一行：
if( nation_res[nation_recno]->race_id != race_id )
    return 0;    // ★ 国家种族 ≠ 单位种族 ⇒ 直接拒绝
```

⇒ 魔族国家想造**人类建筑**时被永久拒绝 ⇒ 没矿场、没兵营、没酒馆 ⇒ 发展链断在第一环。

本项目在 `Nation::ai_should_build_mine` / `ai_should_expand_military` 两处打洞绕过。

**② 叛乱者的决策不看实力**

```c
// Rebel::think_new_action() 原本只有：
switch( misc.random(3) ) { ... }   // 掷骰子决定：攻城 / 打建筑 / 建新城
```

⇒ **2 个叛军也会去攻打有重兵驻守的城堡**。本项目加了兵力分级 + 失败回退链。

**③ "中立"和"会主动攻击"在架构上互斥**

这是本项目**最花时间**才确认的一条：

```c
// Unit::set_attack() 的头两道关卡
if( !can_attack() )                return 0;   // 静默退出
if( !validate_attack_target() )    return 0;   // 静默退出
```

中立巢穴 `nation_recno == 0` ⇒ **没有 Nation 对象** ⇒ 没有外交关系 ⇒ 没有"敌对"状态
⇒ `validate_attack_target()` 必然拒绝 ⇒ **攻击指令被静默吞掉，连日志都不留**。

> **⇒ 任何想"让中立单位主动开战"的尝试都会撞这堵墙。**
> **唯一的架构级解法是给它一个 Nation —— 但 `MAX_NATION = 7`，位子不够。**

**详细推导见 [`docs/05-dead-ends.md`](docs/05-dead-ends.md)。**

### 最有复用价值的：地址表

[`docs/02-addresses.md`](docs/02-addresses.md) 里有完整的：

- **Nation 模块** 9 个函数
- **Rebel 模块** 9 个函数
- **Unit / Firm / Town 结构体偏移**（`soldier_count`、`unit_mode`、`std_hit_points`……）
- **数组对象布局**（`unit_array` / `firm_array` / `town_array` 的真实基址）
- **UnitGroup 整组指令 API**

**每一条都标了"怎么确认的"和"验证状态"** —— 没验证过的会明确写"未验证"。

---

## 安装

### 前置

- **正版《七王国 2 HD》**（Steam 或 GOG 版，2.10F）
- 不需要破解，不需要改 `7K2.exe`

### 三步

1. 把 `release/` 里的补丁包解压到**游戏目录**
2. 双击 `install.bat`
3. 运行 `7K2.exe`

卸载：`uninstall.bat`

> 补丁包里已经包含**上游的简体中文汉化**（见[致谢](#致谢与许可)）。

---

## 从源码编译

```bat
:: 需要 MSVC（本机用 VS BuildTools 的 32 位 x86 工具链）
cd src
build_new.cmd
:: 产物：sk2_zh_new.asi
```

依赖：`user32.lib`、`gdi32.lib`（Windows SDK 自带）。

**注意**：本插件通过 **ASI Loader** 机制加载，代理 DLL 必须叫 `DINPUT.dll`
（**不能**用 `dsound.dll` / `ddraw.dll` —— 它们是 Windows KnownDLL，程序目录的副本会被系统静默忽略）。

---

## AI 开关

改游戏目录里的 `update/render.ini`（**改完存盘，重开游戏生效**）：

```ini
ai=1          # 总开关（管下面两项）
irebel=1      # 叛乱者 AI 改造
airephp=1     # 叛军血量 ×2
airep=1       # 叛军惩罚缩水
ailair=1      # 中立巢穴只读探针（只打日志）
aifort=0      # 巢穴旁造堡垒（实验性，有风险，默认关）
```

---

## 联机

**可以联机，但有前提：**

| 要求 | 说明 |
|---|---|
| 双方装**同一份**补丁 | 同一个 zip、同一个 `.asi` sha256 |
| 双方 `render.ini` 开关一致 | 别一个开一个关 |

**为什么能联机**：7K2 的 AI 随机走**同步种子**（源码里 `LOG_MSG(misc.get_random_seed())` 大量出现，就是给联机排错用的）。
本项目所有冷却都从**真实时间**改成了**游戏内日期**（`info.game_date`），保证两端严格一致。

> ⚠️ **诚实声明：联机尚未实机对局验证。** 以上是架构推理。
> 不同步的表现是**卡住 / 画面漂移**，不是崩溃。若遇到请把双方 `update/sk2_zh.log` 发来。

细节见 [`docs/04-multiplayer.md`](docs/04-multiplayer.md)。

---

## ★ 求帮忙的地方（Roadmap）

这个项目**一个人做了一年**，有几件事明显需要更多人。欢迎 PR / Issue / 讨论：

### 🥇 最想要的：中立巢穴主动出击

**现状**：能让巢穴的兵"出来并移动"，但它们**不会攻击任何人**（见上面第 ③ 条）。

已经打通的：
- ✅ 找到出巢函数 `FirmCamp::patrol()` = `0x00478160`，**中立巢穴可安全调用**
- ✅ 兵真的能出来（实测 15/15、16/16，零丢失）
- ✅ `unit_group.assign()` 能让它们**真的走动**

卡住的：
- ❌ `unit_group.attack()` / 逐单位 `Unit::attack()` **全部静默失败**
- ❌ 根因是 `can_attack()` / `validate_attack_target()` 拒绝中立单位

**求思路**：有没有办法在不给 Nation 的前提下让中立单位获得"敌对"状态？
（改 `validate_attack_target` 的返回值？伪造敌对关系表？还是别的路？）

### 🥈 想做的：源码版移植

7K2 有 **Enlight 官方 2010 年 GPL 源码**（34 万行 C++，已被社区移植到 SDL2）。
但它是 **1.20 老版**，不含 2015 年的 HD 美术。

**求思路**：有没有人试过把 HD 资源塞进源码版？（这是"AI 随便改"和"HD 画面"兼得的唯一路）

### 🥉 其他

- [ ] 魔族**主动提贸易协议**（两处种族闸门已定位，但地址没钉死）
- [ ] 中立巢穴"被打时守军出巢"的函数定位（`patrol()` 四个调用者全在进攻侧，防守是**另一个**函数）
- [ ] 联机实机验证
- [ ] 更多语言的翻译

---

## 项目结构

```
docs/
  01-ai-changes.md     # 五刀改造：改了什么、为什么、效果
  02-addresses.md      # ★ 地址总表（最有价值）
  03-methodology.md    # 怎么从二进制反推语义：有效方法 vs 浪费时间的方法
  04-multiplayer.md    # 联机同步原理
  05-dead-ends.md      # ★ 死路清单：走过的坑
  06-roadmap.md        # 路线图与求助
  07-build.md          # 编译说明
src/                   # 插件源码（ai.cpp 是本项目核心）
release/               # 发布包说明（二进制见 Releases）
```

---

## 致谢与许可

### 这个项目是站在别人肩膀上的

| 谁 | 做了什么 |
|---|---|
| **Enlight Software** | 1999 年做了这个游戏，2010 年以 GPL 放出 7K2 源码 |
| **[HaoJun0823 / SevenKingdom_II_HD_CHINESE](https://github.com/HaoJun0823/SevenKingdom_II_HD_CHINESE)** | **本项目的汉化地基** —— 中文渲染插件（hook 引擎排版链路、字体池、`STD.SET` 数据改写）。我们没有从零造汉化，是在它的基础上**修 bug + 适配 + 加 AI** |
| **7kfans 社区** | 维护 7K2 / 7KAA 的 SDL2 移植 |

**我们做的部分**：修掉上游的文字裁剪 bug（画布 1280×960 判错）、适配 Steam 2.10F 部署链、
五刀 AI 改造、日期显示修复、以及这份逆向文档。

### 许可

**GPLv3**（因为上游汉化插件是 GPLv3，衍生作品必须同许可）。

- 游戏本体、美术资源、数据文件**版权归 Enlight Software**
- 本仓库**不含任何游戏数据**，只含代码和文档
- 使用本补丁前请先拥有正版游戏

---

## English Summary

**Seven Kingdoms II: The Fryhtan Wars (1999) AI overhaul + reverse-engineering notes.**

This repo contains a plugin that fixes three structural flaws in the game's AI:

1. **Fryhtan (monster) nations are permanently blocked from building human structures** — a race check in `Unit::ai_build_firm()` returns 0, so they never get mines, forts, or taverns. Patched at two call sites.
2. **Rebels attack regardless of strength** — their decision is literally `switch(misc.random(3))`. Added troop-count tiers with fallback chains.
3. **Neutral lairs can never fight** — `Unit::set_attack()` silently rejects them because `nation_recno == 0` means no `Nation` object, hence no hostility state. Documented as an architectural wall in `docs/05-dead-ends.md`.

Also includes **a verified address map** for the 2.10F binary (Nation / Rebel / UnitGroup modules,
struct offsets, array layouts) and **a methodology write-up** on reverse-engineering a game without symbols.

**Ships with the upstream GPLv3 Chinese localization plugin**
([HaoJun0823/SevenKingdom_II_HD_CHINESE](https://github.com/HaoJun0823/SevenKingdom_II_HD_CHINESE)) —
we only fixed its clipping bug, adapted it to Steam 2.10F, and added the AI work.

**No game data is included.** Licensed GPLv3.

---

## 更新记录

见 [`CHANGELOG.md`](CHANGELOG.md)。
