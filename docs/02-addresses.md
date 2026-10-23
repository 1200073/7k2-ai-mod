# 地址总表（7K2 HD 2.10F）

> **所有地址都是静态 VA**（ImageBase = `0x400000`）。
> **运行期必须重定位** —— 实测运行时基址不是 `0x400000`：
>
> ```c
> // 静态 VA → 运行期地址
> void* G(uint32_t staticVA) {
>     return (void*)(GetModuleHandle(NULL) + (staticVA - 0x400000));
> }
> ```
>
> ⚠️ **同一份 exe 在不同机器上重定位后基址可能不同**（我们见过 `0x00100000`、`0x00300000`）。
> **永远不要硬编码运行期地址，永远用上面的公式。**

**验证状态说明**

| 标记 | 含义 |
|---|---|
| ✅ | **实机验证通过**（有日志 / 有行为 / 有截图） |
| ⚠️ | 反编译读过，但**没有独立验证** |
| ❓ | 推测，未验证 |

---

## 一、Nation 模块

| 函数 | 静态 VA | 验证 | 备注 |
|---|---|---|---|
| `Nation::process_ai` | **0x00425C70** | ✅ | 序言 `55 8B EC 83 EC ?? 56 89 4D F8`；hook stealSize=10 |
| `Nation::ai_should_build_mine` | **0x0041EDD0** | ✅ | 序言 `55 8B EC 83 EC 14 89 4D FC 8B 45 FC 0F BE 88 DC 0A 00 00`；stealSize=6 |
| `Nation::ai_should_expand_military` | **0x00426AF0** | ✅ | 序言 `55 8B EC 83 EC 20 56 89 4D FC 8B 4D FC E8 ?? ?? ?? ??`；stealSize=7 |
| `Nation::civilian_killed` | **0x00540AF0** | ✅ | 序言 `55 8B EC 51 89 4D FC`；stealSize=7；**`ret 0x0C`（3 个栈参数）** |
| `Nation::change_reputation` | **0x0053FDB0** | ⚠️ | 从 `civilian_killed` 反编译读到的调用目标 |
| `Nation::change_all_people_loyalty` | **0x00540B60** | ⚠️ | 同上 |
| `Nation::think_military` | 0x004269A0 | ⚠️ | 遍历 `ai_town_array` 挑城镇 |
| `Unit::update_being_killed_statistic` | **0x005E3440** | ✅ | 序言 `55 8B EC 83 EC 18 89 4D F8`；stealSize=9 |

### 🔑 `civilian_killed` 是怎么找到的（方法论范例）

全库搜索**同时引用两个浮点常量**的函数：

```c
// 该函数体内同时用了 -10.0f 和 -1.0f
change_reputation( killedCount * -10.0f );   // 商队
change_reputation( killedCount *  -1.0f );   // 平民/叛军
```

⇒ **全 exe 只有一个函数同时引用这两个常量** ⇒ `0x540AF0`，一次命中。

**⇒ 详见 [`03-methodology.md`](03-methodology.md)。**

---

## 二、Rebel 模块

| 函数 | 静态 VA | 备注 |
|---|---|---|
| `Rebel::next_day` | 0x00563170 | 每日驱动 |
| `Rebel::think_new_action` | **0x00563390** | ★ 本项目改这里；序言 `55 8B EC 83 EC 0C 89 4D FC`；stealSize=9 |
| `Rebel::think_cur_action` | 0x00563410 | |
| `Rebel::think_town_action` | 0x005634B0 | **死代码**（没人调用） |
| `Rebel::think_settle_new` | 0x005634F0 | 建新叛军城 |
| `Rebel::think_settle_to` | 0x00563610 | |
| `Rebel::think_capture_attack_town` | 0x00563790 | |
| `Rebel::think_attack_firm` | 0x00563930 | |
| `Rebel::execute_new_action` | **0x00563AC0** | ★★ 整组指令的**教科书**：`zap → add_unit → attack/assign` |
| `Rebel::turn_indepedent` | 0x00563DF0 | |
| `Rebel::set_action` | 0x00564700 | |

### `Rebel` 结构体偏移

| 字段 | 偏移 | 类型 |
|---|---|---|
| `rebel_recno` | +0x00 | short |
| `leader_unit_recno` | +0x02 | short |
| `action_mode` | +0x04 | char |
| `action_para` | +0x05 | short |
| `action_para2` | +0x07 | short |
| `mobile_rebel_count` | +0x09 | short |
| `town_recno` | +0x0B | short |
| `hostile_nation_bits` | +0x0D | char |

> **`execute_new_action` 的 `action_mode` 取值**：
> `2` = 攻城镇、`3` = 攻建筑、`4` = 建新城、`5` = 迁移（**内部就是一句 `unit_group.assign`，无任何前置判断**）

---

## 三、Firm / FirmCamp 模块

| 函数 | 静态 VA | 验证 | 备注 |
|---|---|---|---|
| **`FirmCamp::patrol(short)`** | **0x00478160** | ✅ | ★ 把库存兵变成地图单位；序言 `55 8B EC 83 EC 1C 56 89 4D F8 8B 45 F8 0F BE 48` |
| `FirmCamp::patrol_all_soldier` | 0x004783E0 | ⚠️ | 序言 `55 8B EC 83 EC 10 56 89 4D F8 C7 45 F4 01 00 00` |
| `FirmCamp::assign_overseer` | 0x00485240 | ⚠️ | 序言 `55 8B EC 83 EC 30 89 4D FC 83 7D 08 00 75 13 8B` |
| `FirmCamp::mobilize_soldier` | ❓ 未定位 | — | 源码在 `of_campu.cpp:431`；**单兵动员**，`patrol` 内部调它 |

### 🔑 `patrol()` 为什么对中立巢穴安全

```c
void FirmCamp::patrol(short patrolType)
{
    if( nation_recno == nation_array.player_recno )   // ★ 只读 player_recno
        power.reset_selection();
    ...
}
```

**⇒ 它只读 `nation_array.player_recno` 这一个全局，从不写 `nation_array[nation_recno]`**
⇒ **`nation_recno == 0` 的巢穴也能安全调用。**

**但**：它只负责"把兵放出来"，**不给任何目标指令** ⇒ 兵出来后没有归宿。
（详见 [`05-dead-ends.md`](05-dead-ends.md)）

---

## 四、UnitGroup（整组指令 API）

| 方法 | 静态 VA | 参数 | 验证 |
|---|---|---|---|
| `unit_group`（全局对象） | **0x0069CAE4** | — | ✅ |
| `zap()` 清空 | 0x00469F70 | 1（传 1） | ✅ |
| `add_unit(recno)` | 0x005E6830 | 1 | ✅ |
| `attack(recno, cmd)` | **0x005E6B50** | 2 | ⚠️ 调用成功但**对中立单位静默失效** |
| `assign(x, y, land, cmd, a5, a6)` | **0x005E6CE0** | 6 | ✅ **实测能让单位走动** |
| `get_unit(n)` | 0x005E6870 | 1 | ⚠️ |

**`assign` 的实测参数**：`assign(x, y, UNIT_LAND=0x4C, COMMAND_AI=2, -1, 0)`

**怎么找到这些地址的**：反查 `execute_new_action`（0x563AC0）里各 `call` 前 `mov ecx, imm32` 的来源。

---

## 五、数组全局（★ 踩过大坑）

### 基址

| 数组 | 静态 VA | 验证 |
|---|---|---|
| `unit_array` | **0x0069CAB4** | ✅ |
| `firm_array` | **0x0069FB64** | ✅ |
| `town_array` | **0x00688700** | ✅ |

> ### ⚠️⚠️ 血泪教训：`unit_array` 的基址**不是** `0x69CAB0`
>
> 引擎里 `size()` 用 `0x69CAB4`，`operator[]` 用 `0x69CAB0` —— 差 4 字节。
> 我当初随便挑了一个，**用错的地址读了三个月**：
>
> ```
> 0x69CAB0:  count=1   elem=160  data=0x00FFFFFF   ← 全是垃圾/哨兵值
> 0x69CAB4:  count=160 elem=4    data=0x0E1D3678   ← 全对
> ```
>
> **后果：三次游戏崩溃，烧掉几十 M token，而且把根因误判成"调用契约问题"。**
>
> **定位方法**：拿**已知正确的 `firm_array`（0x69FB64）当尺子**，并排 dump 原始内存，
> 一眼就看出整体错位 4 字节。

### Array 对象布局

**1-based 索引，越界返回 0：**

```c
// Array::operator[] @ 0x41C250 的实现（反编译确认）
if( i < 1 || count < i ) return 0;
return *(void**)( (i-1) * elem_size + data );

// 字段：
//   count     @ +0x0C  (int)
//   elem_size @ +0x10  (int)
//   data      @ +0x19  (uint32，**未对齐**)
```

> ### ⚠️ `count` 不是"存活数量"
>
> **`unit_array.count` 是"最高已用编号"** —— 单位死亡后槽位会被**复用**。
> 新建 15 个单位，`count` 可能一点不涨（全填旧空位）。
>
> **⇒ 不要用 `count` 的增量判断"创建了几个"。**

### 想找"新建了哪些单位"怎么办

**不要扫 `unit_array`，去读生产方自己记的句柄列表。**

以 `patrol()` 为例：

```c
FirmCamp::patrol_all_soldier() {
    unitRecno = mobilize_soldier(id, COMMAND_AUTO);
    patrol_unit_array[patrol_unit_count++] = unitRecno;   // ★ 新建的 recno 记在这里
    if(!unitRecno) return 0;
    ...
}
```

| 字段 | 偏移 | 含义 |
|---|---|---|
| `FirmCamp::patrol_unit_count` | **Firm+0x3D1**（char） | ⚠️ **尝试次数**，失败也记一笔 |
| `FirmCamp::patrol_unit_array` | **Firm+0x3D2**（short[]） | ★ **新建单位的 recno** |

**⇒ 这条经验通用：生产方自己记的句柄列表，永远比"某个计数器的增量"可信。**

---

## 六、结构体偏移

### BaseObj（所有可攻击对象的基类，`pack(1)`）

| 字段 | 偏移 | 类型 | 验证 |
|---|---|---|---|
| vtable | +0x00 | ptr | — |
| **`base_obj_recno`** | **+0x04** | short | ✅ 源码确认是 vtable 后第一个成员 |
| **`nation_recno`** | **+0x0B** | char | ✅ |

### Firm / FirmCamp

| 字段 | 偏移 | 类型 | 验证 |
|---|---|---|---|
| `loc_x1` / `loc_y1` | +0x23 / +0x25 | short | ✅ |
| **`firm_id`** | **+0x42** | char | ✅（`FIRM_LAIR = 12`） |
| **`overseer_recno`** | **+0x128** | short | ✅ |
| **`soldier_count`** | **+0x3CD** | char | ✅ 上限 `MAX_SOLDIER = 15` |
| `patrol_unit_count` | +0x3D1 | char | ✅ |
| `patrol_unit_array` | +0x3D2 | short[] | ✅ |

> **`soldier_count` 是怎么找到的**：`ai_should_expand_military` 反编译里有
> `9 - (*(char*)(f+0x3cd) + (0<*(short*)(f+0x128)) + *(char*)(f+0x3d1))`
> ⇒ 一眼认出 `0x3CD` = 兵数、`0x128` = 监工、`0x3D1` = patrol 计数。

### Unit

| 字段 | 偏移 | 类型 | 验证 |
|---|---|---|---|
| `nation_recno` | +0x0B | char | ✅ |
| `race_id` | +0x0C | char | ✅ |
| **`skill`** | **+0x7B** | `Skill`（15 字节，pack(1)） | ✅ |
| **`skill.std_hit_points`** | **+0x80** | short | ✅ ★ 改这个 = 改血量 |
| `unit_mode` | **+0x8A** | char | ✅ |
| `unit_mode_para` | **+0x8B** | short | ✅ |
| `team_info` | +0x91 | ptr | ⚠️ |
| `leader_unit_recno` | +0x95 | short | ⚠️ |

**`Skill` 结构（`pack(1)`，15 字节）**：

```
skill_potential(1) + max_combat_level(2) + max_skill_level(2) +
std_hit_points(2) + combat_level(4) + skill_level(4) = 15
```

**`unit_mode` 取值**：

| 值 | 含义 |
|---|---|
| 6 | `UNIT_MODE_REBEL`（叛军） |
| 7 | `UNIT_MODE_MONSTER`（已出巢的野怪） |

> ### 💡 内联函数没法 hook —— 改字段
>
> `Unit::max_hit_points()` **是头文件内联函数**，exe 里**没有独立入口**可供 hook。
> **解法：直接改它算的那个字段** `skill.std_hit_points`。
> 数据胜过代码 —— 本项目"叛军血量 ×2"就是这么实现的（实测 `200 -> 400`）。

---

## 七、其他关键全局

| 项 | 静态 VA | 验证 | 说明 |
|---|---|---|---|
| **`info.game_date`** | **0x0068E334** | ✅ | 游戏内日期，**联机两端一致** |
| `nation_array.player_recno` | 0x0069DBB2（char） | ✅ | 玩家国家号 |
| `misc.random(n)` | 0x00530E30 | ✅ | **同步随机**（联机安全） |

**`Info` 结构（`pack(1)`，关键字段）**：

```c
class Info {
    int game_start_date;   // +0x00
    int game_date;         // +0x04  ★ 每天 +1
    int game_day;          // +0x08
    int game_month;        // +0x0C
    int game_year;         // +0x10
    ...
    int random_seed;       // ★ 种子也在里面 ⇒ 同步状态的一部分
};
```

> **`info.game_date` 的用途**：**任何"冷却""间隔""节流"都必须用它，不能用 `GetTickCount()`。**
> 真实时间在两台机器上永远不同 ⇒ 联机会不同步。
>
> 怎么确认它就是 `game_date`：`Nation::process_ai` 里它被反复当"当前天"用：
> `if( (DAT_0068e334 - nationRecno) % 3 == 0 )`（每 3 天）、
> `if( DAT_0068e334 % 360 == nationRecno % 360 )`（每 360 天）

---

## 八、编译期常量（来自 GPL 源码）

这些**改不了**（编译期写死在 exe 里），但理解它们很重要：

```c
// gamedef.h
#define MAX_NATION         7    // ★ 国家位上限（玩家 + AI + 魔族 共享）
#define MAX_COLOR_SCHEME   7    // 只有 7 套国家配色
#define MAX_PLAYER         7
#define MAX_RACE          12
#define MAX_SOLDIER       15    // 每个营地/巢穴的最多兵数
```

**⇒ `MAX_NATION = 7` 是硬墙**：第 8 个国家会同时踩穿十几处按国家号下标的数组，
最狠的是 `get_relation()`：

```c
NationRelation* get_relation(int n) { return relation_array + n - 1; }   // 零边界检查
```

**细节见 [`05-dead-ends.md`](05-dead-ends.md) 的"第 8 个国家"那一节。**

---

## 九、怎么自己验证

**不要相信这张表 —— 自己验一遍。**

```c
// 1) 字节校验（最基础的一层保险）
static bool CheckBytes(uint32_t va, const uint8_t* sig, int len) {
    const uint8_t* p = (const uint8_t*)G(va);
    for (int i = 0; i < len; ++i)
        if (p[i] != sig[i]) return false;
    return true;
}

// 2) 地址对不对，打印一下就知道
//    firm_array count 应该是几十~几百，elem 必须是 4，data 必须是合法堆指针
```

**本项目所有 hook 都带字节指纹校验** —— 校验不过就跳过，绝不盲调。

---

## 参考

- 源码树（GPL，1.20 老版）：`https://7kfans.com/git/?p=7k2.git`
- 本项目方法论：[`03-methodology.md`](03-methodology.md)
- 走过的坑：[`05-dead-ends.md`](05-dead-ends.md)
