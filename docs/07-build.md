# 编译说明

## 环境

| 项 | 要求 |
|---|---|
| 编译器 | **MSVC（x86 / 32 位）** —— 本项目用 Visual Studio BuildTools |
| 目标平台 | **32 位（x86）** —— 因为 `7K2.exe` 是 32 位 |
| SDK | Windows SDK（`user32.lib`、`gdi32.lib` 自带） |

> ⚠️ **必须 32 位。** 64 位编译出来的 DLL 游戏加载不了。

## 编译

```bat
cd src
build_new.cmd
```

产物：`sk2_zh_new.asi`

## 编译参数（`build_new.cmd` 里）

```bat
set CFLAGS=/nologo /c /EHsc /std:c++17 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /O2 /Oi
```

**注意几处**：

| 参数 | 为什么 |
|---|---|
| `/utf-8` | 源码里有中文注释和字符串 |
| `/EHsc` | 异常处理（`__try/__except` 用于安全读内存） |
| **没有 `/Gr`** | ★ 默认 `__cdecl` —— 所以 hook 代理**必须显式声明 `__fastcall`** |

## ⚠️ ABI 陷阱（最容易踩的坑）

**目标函数是 `__thiscall`（`this` 在 ECX），代理必须声明 `__fastcall`。**

```c
// ❌ 错：cdecl 代理，会从栈上读垃圾 self
static void __cdecl Hook(void* self) { ... }

// ✅ 对
static void __fastcall Hook(void* self, void* edx) { ... }
```

**踩过的表现**：读出来的 `self` 是一堆垃圾指针，vtable 指向 `"RESE"/"_COM"` 这种乱码。

## 部署

```bat
:: 1. 关掉游戏（文件被占用时复制会静默失败）
:: 2. 复制到游戏目录
copy sk2_zh_new.asi "<游戏目录>\scripts\sk2_zh.asi"

:: 3. 确认 DINPUT.dll 在游戏根目录（ASI Loader 代理）
```

> ⚠️ **代理 DLL 必须叫 `DINPUT.dll`。**
> **不能**用 `dsound.dll` / `ddraw.dll` —— 它们是 Windows **KnownDLL**，
> 程序目录的副本会被系统**静默忽略**，插件根本不会加载。

## 验证装没装上

看游戏目录的 `update\sk2_zh.log`：

```
[O] 魔族开矿补丁已安装：有城镇的魔族(除牛头人)可建矿场
[O] 魔族扩军补丁已安装：有城镇且无兵营的魔族会造堡垒(FORT)
[O] 叛乱者 AI 改造已安装：弱队不攻城 / 强队才攻城 / 可自己建新城
[O] 叛军惩罚补丁已安装：杀叛军声誉仅 -0.3/人，忠诚不变
```

**有 `[O]` 就是装上了。**

## 调试

```c
// 本项目自带的诊断日志（只读，不影响游戏）
Log("[AI] 魔族豁免生效 #%ld: monster=%d 城镇=%d 矿=%d 现金=%.0f\r\n", ...);
```

**首次命中才打，避免刷屏。**

## 字节指纹校验

**所有 hook 都带校验：**

```c
static const uint8_t SIG[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
if (!CheckBytes(VA, SIG, 6)) { Log("[X] 校验失败"); return; }
```

**⇒ 版本不匹配时不会崩，只会安静地不生效。**

**如果你换了个游戏版本（不是 2.10F），大概率所有校验都会失败 —— 那时需要重新定位地址。**