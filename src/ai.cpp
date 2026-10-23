// ============================================================================
// ai.cpp — 魔族开矿补丁（"第 1 刀"）
//
// 目标：让魔族（Fryhtan）在**拥有城镇**之后，能够像人类一样建造矿场。
//
// 背景（全部有源码/反编译双重依据）：
//   Nation::ai_should_build_mine() 的第一道硬门槛是
//       if( total_jobless_population < 16 ) return 0;
//   而 total_jobless_population 只累加"自己拥有的城镇的失业人口"
//   （onationa.cpp:727），魔族开局只有巢穴 ⇒ 恒为 0 ⇒ 永远不造矿。
//
// 本补丁不改原函数，而是**在它返回 0 之后**追加一条"魔族豁免"判断。
// 人族走原逻辑，行为零变化。
//
// 硬约束（按用户要求）：
//   1. 不影响人族
//   2. 排除牛头人（MINOTAUROS）—— 它有炼金术，靠炼金产生命点数，不需要矿
//   3. 不能一上来就开矿 —— 必须有城镇（ai_town_count > 0）
//
// 地址与偏移（Steam 2.10F，ImageBase 0x400000）：
//   ai_should_build_mine  VA 0x0041EDD0 / RVA 0x1EDD0
//   序言 55 8B EC 83 EC 14（6 字节）= push ebp; mov ebp,esp; sub esp,0x14
//   第 3 条指令 0F BE 88 DC 0A 00 00 = movsx ecx, byte [eax+0xADC]
//                                     （pref_economic_development，佐证偏移正确）
// ============================================================================

#include "core.h"

#include <cstdio>
#include <cstring>

namespace sk2 {
namespace {

// ---- 地址 ----
constexpr uint32_t RVA_AI_SHOULD_BUILD_MINE = 0x1EDD0;

// 完整签名（用于确认"钩到的是对的函数"，而不是别的）
const uint8_t SIG_AI_SHOULD_BUILD_MINE[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14,            // push ebp; mov ebp,esp; sub esp,14h
    0x89, 0x4D, 0xFC,                              // mov [ebp-4], ecx
    0x8B, 0x45, 0xFC,                              // mov eax, [ebp-4]
    0x0F, 0xBE, 0x88, 0xDC, 0x0A, 0x00, 0x00,      // movsx ecx, byte [eax+0ADC h]
    0xB8, 0x64, 0x00, 0x00, 0x00                   // mov eax, 64h (=100)
};

// ---- Nation 对象偏移（pack(1) 布局 + 反编译双重确认）----
constexpr int OFF_NATION_RECNO   = 0x004;   // short nation_recno
constexpr int OFF_RACE_ID        = 0x009;   // char  race_id      <0 = 魔族, >0 = 人族
constexpr int OFF_CASH           = 0x075;   // float cash
constexpr int OFF_AI_TOWN_COUNT  = 0xA3B;   // short ai_town_count
constexpr int OFF_AI_CAMP_COUNT  = 0xA3F;   // short ai_camp_count
constexpr int OFF_AI_MINE_COUNT  = 0xA41;   // short ai_mine_count

// ---- 地址 ----
constexpr uint32_t RVA_AI_SHOULD_EXPAND_MILITARY = 0x26AF0;   // VA 0x00426AF0
constexpr uint32_t RVA_THINK_BUILD_FIRM          = 0x1E7A0;   // VA 0x0041E7A0

// ai_camp_array / ai_camp_count
constexpr int OFF_AI_CAMP_ARRAY  = 0x9B9;

// ---- 常数 ----
constexpr int   MONSTER_MINOTAUROS = 4;         // omonsres.h: KHARSHUF=1 ... EXOVUM=7
constexpr float MINE_SETUP_COST    = 600.0f;    // STD.SET FIRM 表实测（Mine setup_cost）

typedef int (__thiscall* FnAiShouldBuildMine)(void* self);

FnAiShouldBuildMine g_orig = nullptr;
bool                g_enabled = true;

volatile LONG g_callCount = 0;    // 原函数被调用次数
volatile LONG g_monsterCall = 0;  // 其中魔族的次数
volatile LONG g_overrideHit = 0;  // 豁免生效次数
volatile LONG g_logged = 0;       // 已打印的详细日志条数

// 扩军补丁（2a）的统计量，提前声明以便统计函数引用
volatile LONG g_miliCall = 0;
volatile LONG g_miliMonster = 0;
volatile LONG g_miliHit = 0;
volatile LONG g_miliLogged = 0;

// ---- "慢慢经营"：每国上次批准造矿的时刻 ----
constexpr int   MAX_NATION_SLOTS   = 16;
// 静态 VA → 运行期地址（必须重定位：实测运行时基址不是 0x400000）
static inline void* G(uint32_t staticVA)
{
    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(Base()) + (staticVA - 0x400000u));
}
// ★ 联机安全：冷却必须用「游戏内日期」而不是真实时间 —— 墙钟在两台机器上永远不同，
//   冷却边界处会一边通过一边不通过 ⇒ AI 决策分歧 ⇒ 联机不同步。
//   info.game_date 是两端严格一致的游戏状态（Info 是 pack(1)，game_start_date@+0、game_date@+4）。
constexpr uint32_t VA_INFO_GAME_DATE  = 0x0068E334;   // info.game_date（由 Nation::process_ai 反编译确认）
constexpr int      MINE_COOLDOWN_DAYS = 30;           // 30 个游戏日 ≈ 原 120 秒的观感
static int g_lastMineDay[MAX_NATION_SLOTS] = { 0 };

static inline int GameDateNow()
{
    return *reinterpret_cast<const int*>(G(VA_INFO_GAME_DATE));
}

// ---------------------------------------------------------------------------
// ★ 身份校验：hook 入口先确认 ECX 真的是一个 Nation 对象
//
// 起因（2026-10-17 实机取证）：0xC0000005 落在 RVA 0x1EDDC = ai_should_build_mine + 0xC，
// 正是该函数第一次解引用 this 的指令（movsx ecx, byte [eax+0ADCh]）。
// 说明有调用路径把空/坏指针当 this 传进来。
//
// 对策：不合法就**不调用原函数、直接 return 0**，让那条解引用永远不会执行。
// 代价：极少数情况本可正常返回的结果会被当成 0 —— 但 0 就是原函数绝大多数时候的返回值，影响可忽略。
// ---------------------------------------------------------------------------
static bool LooksLikeNation(const void* self)
{
    if (!self)
        return false;
    const uint8_t* n = static_cast<const uint8_t*>(self);
    if (reinterpret_cast<uintptr_t>(n) < 0x10000)
        return false;
    const short recno = *reinterpret_cast<const short*>(n + OFF_NATION_RECNO);
    if (recno < 1 || recno >= MAX_NATION_SLOTS)
        return false;
    const signed char race = *reinterpret_cast<const signed char*>(n + OFF_RACE_ID);
    if (race < -8 || race > 10)        // MAX_MONSTER_TYPE=7, MAX_RACE≈9
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// 魔族豁免判断：满足则允许建矿
// ---------------------------------------------------------------------------
// ★ 一次性诊断：把真实 Nation 对象的 vtable 及其前几个槽位打印出来
//   （用于定位 think_diplomacy —— 静态搜 vtable 推出的 [+0x20] 与实际反编译矛盾，
//     改由运行期自报，零推测）
static volatile LONG g_vtLogged = 0;

// ★ 安全读取：任何非法地址都返回 0，绝不把异常抛出去
//   （2026-10-17 曾因裸解引用 vtable 导致 0xC0000005 @ RVA 0xB35C）
static uint32_t SafeRead32(uintptr_t addr)
{
    if (addr < 0x10000 || addr > 0xFFE00000u)
        return 0;
    __try {
        return *reinterpret_cast<const uint32_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void LogNationVtableOnce(const void* self)
{
    if (InterlockedIncrement(&g_vtLogged) != 1)
        return;
    const uint32_t vt = SafeRead32(reinterpret_cast<uintptr_t>(self));
    uint32_t e14 = 0, e18 = 0, e1c = 0, e20 = 0, e24 = 0, e28 = 0;
    if (vt) {
        e14 = SafeRead32(vt + 0x14);
        e18 = SafeRead32(vt + 0x18);
        e1c = SafeRead32(vt + 0x1C);
        e20 = SafeRead32(vt + 0x20);
        e24 = SafeRead32(vt + 0x24);
        e28 = SafeRead32(vt + 0x28);
    }
    Log("[VT] this=0x%08X vtable=0x%08X\r\n", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), vt);
    Log("[VT2] [+14]=0x%08X [+18]=0x%08X [+1C]=0x%08X [+20]=0x%08X [+24]=0x%08X [+28]=0x%08X\r\n",
        e14, e18, e1c, e20, e24, e28);
    Log("[VT2] 校验: [+14]应=00425C70(%s)  [+18]应=00425EC0(%s)\r\n",
        (e14 == 0x00425C70u) ? "是" : "否", (e18 == 0x00425EC0u) ? "是" : "否");
}

int MonsterMineOverride(void* self)
{
    // ★ 身份校验：坏 this 直接返回 0，绝不进原函数（防 RVA 0x1EDDC 那类崩溃）
    if (!LooksLikeNation(self))
        return 0;

    LogNationVtableOnce(self);

    int r = g_orig ? g_orig(self) : 0;
    if (r)
        return r;                       // 原函数已同意 —— 直接放行（含人族全部情况）

    InterlockedIncrement(&g_callCount);

    const uint8_t* n = static_cast<const uint8_t*>(self);

    // ① 只对魔族生效
    const signed char raceId = *reinterpret_cast<const signed char*>(n + OFF_RACE_ID);
    if (raceId >= 0)
        return 0;                       // 人族：原样返回 0，行为零变化

    InterlockedIncrement(&g_monsterCall);

    const int monsterId = -raceId;

    // ② 排除牛头人（有炼金术，不走这条路）
    if (monsterId == MONSTER_MINOTAUROS)
        return 0;

    // ③ 必须先有城镇（避免"一上来就开矿"）
    const short townCount = *reinterpret_cast<const short*>(n + OFF_AI_TOWN_COUNT);
    if (townCount <= 0)
        return 0;

    // ④ 先开一个慢慢经营：同一国家两次批准之间要有冷却，
    //    避免"3 个城就同时开 3 个矿"那种乱开（用户反馈 2026-10-17）
    const short nationRecno = *reinterpret_cast<const short*>(n + OFF_NATION_RECNO);
    if (nationRecno > 0 && nationRecno < MAX_NATION_SLOTS) {
        const int today = GameDateNow();
        if (g_lastMineDay[nationRecno] != 0 &&
            today - g_lastMineDay[nationRecno] < MINE_COOLDOWN_DAYS)
            return 0;
        g_lastMineDay[nationRecno] = today;
    }

    // ⑤ 每个城镇最多一个矿
    const short mineCount = *reinterpret_cast<const short*>(n + OFF_AI_MINE_COUNT);
    if (mineCount >= townCount)
        return 0;

    // ⑤ 钱够（矿场造价 600）
    const float cash = *reinterpret_cast<const float*>(n + OFF_CASH);
    if (cash < MINE_SETUP_COST)
        return 0;

    // ---- 通过 ----
    LONG hit = InterlockedIncrement(&g_overrideHit);
    LONG lg  = InterlockedIncrement(&g_logged);
    if (lg <= 20) {
        Log("[AI] 魔族豁免生效 #%ld: monster=%d 城镇=%d 矿=%d 现金=%.0f\r\n",
            hit, monsterId, (int)townCount, (int)mineCount, cash);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 极简 INI 读取：在 render.ini 里找 `ai=0`（其它值/缺省 = 开启）
// ---------------------------------------------------------------------------
// 通用键读取：在 render.ini 里找 `key=N`
static int ReadIntSwitch(const char* key, int defaultValue)
{
    static const char* kPaths[] = { "update\\render.ini", "scripts\\render.ini", "render.ini" };
    const size_t klen = strlen(key);
    for (const char* p : kPaths) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") != 0 || !f)
            continue;
        char line[512];
        int result = defaultValue;
        while (fgets(line, sizeof(line), f)) {
            char* s = line;
            while (*s == ' ' || *s == '\t') ++s;
            if (_strnicmp(s, key, klen) == 0 && s[klen] == '=')
                result = atoi(s + klen + 1);
        }
        fclose(f);
        return result;
    }
    return defaultValue;
}

bool ReadAiSwitch()
{
    return ReadIntSwitch("ai", 1) != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
bool InstallMonsterMine()
{
    g_enabled = ReadAiSwitch();
    if (!g_enabled) {
        Log("[i] render.ini 里 ai=0，跳过魔族开矿补丁\r\n");
        return false;
    }

    // ---- 定位（签名优先，RVA 兜底，且必须字节校验通过）----
    void* target = FindInText(SIG_AI_SHOULD_BUILD_MINE, sizeof(SIG_AI_SHOULD_BUILD_MINE));
    bool bySig = (target != nullptr);

    if (!target) {
        target = Rva(RVA_AI_SHOULD_BUILD_MINE);
        if (!InImageRange(target, sizeof(SIG_AI_SHOULD_BUILD_MINE)) ||
            memcmp(target, SIG_AI_SHOULD_BUILD_MINE, sizeof(SIG_AI_SHOULD_BUILD_MINE)) != 0) {
            Log("[X] ai_should_build_mine 定位失败（签名未命中，RVA 处字节也不符）\r\n");
            return false;
        }
        Log("[!] ai_should_build_mine 签名未命中，改用 RVA 兜底（字节校验通过）\r\n");
    }

    const uint32_t va = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(Base()) + 0x400000u);
    Log("[i] ai_should_build_mine @ %08X（%s）\r\n", va, bySig ? "签名" : "RVA兜底");

    // ---- 先建跳板（必须在打补丁前，跳板要复制原函数头部）----
    g_orig = reinterpret_cast<FnAiShouldBuildMine>(BuildTrampoline(target, 6));
    if (!g_orig) {
        Log("[X] 跳板创建失败，放弃本次补丁（不冒险裸钩）\r\n");
        return false;
    }

    // ---- 打 JMP ----
    uint8_t saved[5] = { 0 };
    if (!InstallJump(target, reinterpret_cast<void*>(&MonsterMineOverride), saved)) {
        Log("[X] 安装 JMP 失败\r\n");
        g_orig = nullptr;
        return false;
    }

    Log("[O] 魔族开矿补丁已安装：有城镇的魔族(除牛头人)可建矿场\r\n");
    Log("    规则: 城镇数>0 / 矿数<城镇数 / 现金>=%.0f\r\n", MINE_SETUP_COST);
    return true;
}

// ---------------------------------------------------------------------------
// 诊断：供 diag / 退出时打印
// ---------------------------------------------------------------------------
void MonsterMineStats()
{
    Log("[AI] 统计: 原函数返回0次数=%ld 其中魔族=%ld 豁免生效=%ld\r\n",
        g_callCount, g_monsterCall, g_overrideHit);
    Log("[AI] 扩军统计: 原函数0次=%ld 魔族=%ld 豁免=%ld\r\n",
        g_miliCall, g_miliMonster, g_miliHit);
}

// ===========================================================================
// 补丁 2a：魔族扩军豁免（让魔族会盖堡垒）
//
// 背景：
//   Nation::think_military() 本来就会挑城镇 → find_best_place_loc(FIRM_FORT)
//   → add_action(... FIRM_FORT)，但它先看 ai_should_expand_military()：
//
//     if( true_profit_365days() < 0 ) return 0;
//     if( !ai_should_spend(...) )     return 0;
//     if( ai_camp_count*9 > total_jobless_population*(50+pref_military)/150 )
//         return 0;      // ★★ 魔族 total_jobless_population 恒为 0 ⇒ 只要有兵营就永远不扩
//
//   ⇒ 只要让这个函数对魔族放行，原版剩下的活（挑城/选址/下命令）全自动完成。
//
// 地址：ai_should_expand_military  VA 0x00426AF0  (RVA 0x26AF0)
//       序言 55 8B EC 83 EC 20 56 = push ebp; mov ebp,esp; sub esp,20h; push esi  (7 字节)
// ===========================================================================

typedef int (__thiscall* FnAiShouldExpandMilitary)(void* self);

static FnAiShouldExpandMilitary g_miliOrig = nullptr;

static constexpr float FORT_SETUP_COST = 400.0f;   // STD.SET FIRM 表实测（Fort setup_cost）

static int MonsterMilitaryOverride(void* self)
{
    // ★ 身份校验（同 Mine 补丁）
    if (!LooksLikeNation(self))
        return 0;

    int r = g_miliOrig ? g_miliOrig(self) : 0;
    if (r)
        return r;                              // 原逻辑已同意 → 放行

    InterlockedIncrement(&g_miliCall);

    const uint8_t* n = static_cast<const uint8_t*>(self);

    const signed char raceId = *reinterpret_cast<const signed char*>(n + OFF_RACE_ID);
    if (raceId >= 0)
        return 0;                              // 人族：原样返回 0，行为零变化

    InterlockedIncrement(&g_miliMonster);

    if (-raceId == MONSTER_MINOTAUROS)
        return 0;                              // 牛头人：经济先不动

    const short townCount = *reinterpret_cast<const short*>(n + OFF_AI_TOWN_COUNT);
    if (townCount <= 0)
        return 0;                              // 没城镇：think_military 也挑不到城，白放行

    const short campCount = *reinterpret_cast<const short*>(n + OFF_AI_CAMP_COUNT);
    if (campCount > 0)
        return 0;                              // 已经有兵营：不重复要（让原逻辑接手后续扩军）

    const float cash = *reinterpret_cast<const float*>(n + OFF_CASH);
    if (cash < FORT_SETUP_COST)
        return 0;

    LONG hit = InterlockedIncrement(&g_miliHit);
    LONG lg  = InterlockedIncrement(&g_miliLogged);
    if (lg <= 20) {
        Log("[AI] 扩军豁免生效 #%ld: monster=%d 城镇=%d 兵营=%d 现金=%.0f\r\n",
            hit, -raceId, (int)townCount, (int)campCount, cash);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 安装补丁 2a
// ---------------------------------------------------------------------------
bool InstallMonsterMilitary()
{
    if (!ReadAiSwitch()) {
        Log("[i] render.ini 里 ai=0，跳过魔族扩军补丁\r\n");
        return false;
    }

    // 签名：只取到 call 指令的操作码 E8，rel32 用通配（跨版本会变）
    static const char* PAT = "55 8B EC 83 EC 20 56 89 4D FC 8B 4D FC E8 ?? ?? ?? ??";
    void* target = FindInTextPat(PAT);
    bool bySig = (target != nullptr);

    if (!target) {
        target = Rva(RVA_AI_SHOULD_EXPAND_MILITARY);
        static const uint8_t HEAD[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x20,0x56,0x89,0x4D,0xFC,0x8B,0x4D,0xFC };
        if (!InImageRange(target, sizeof(HEAD)) || memcmp(target, HEAD, sizeof(HEAD)) != 0) {
            Log("[X] ai_should_expand_military 定位失败\r\n");
            return false;
        }
        Log("[!] ai_should_expand_military 签名未命中，改用 RVA 兜底（字节校验通过）\r\n");
    }

    const uint32_t va = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(Base()) + 0x400000u);
    Log("[i] ai_should_expand_military @ %08X（%s）\r\n", va, bySig ? "签名" : "RVA兜底");

    g_miliOrig = reinterpret_cast<FnAiShouldExpandMilitary>(BuildTrampoline(target, 7));
    if (!g_miliOrig) {
        Log("[X] ai_should_expand_military 跳板创建失败\r\n");
        return false;
    }

    uint8_t saved[5] = { 0 };
    if (!InstallJump(target, reinterpret_cast<void*>(&MonsterMilitaryOverride), saved)) {
        Log("[X] ai_should_expand_military 安装 JMP 失败\r\n");
        g_miliOrig = nullptr;
        return false;
    }

    Log("[O] 魔族扩军补丁已安装：有城镇且无兵营的魔族会造堡垒(FORT)\r\n");
    return true;
}

// ===========================================================================
// 补丁 2a-2：魔族"巢穴旁造堡垒"（解决只有人类士兵时的死锁）
//
// 背景：只有人类士兵、没有城镇时
//   Unit::think_normal_human_action() 里
//     ai_build_firm()  ← 被种族检查挡死（魔族国 + 人类单位）
//     士兵 → think_assign_soldier_to_camp()  ← 要兵营，而兵营建不出来
//   ⇒ 死锁。
//
// 本补丁在国家级 think_build_firm 里补一句：没有城镇但有巢穴的魔族，
// 直接在巢穴旁边下一条 FIRM_FORT 命令 ⇒ 士兵有营可进（将军位出现）。
//
// 依赖（全部已确认调用约定）：
//   Nation::think_build_firm      0x0041E7A0  序言 55 8B EC 51 89 4D FC（偷 7 字节）
//   Nation::find_best_place_loc   0x00427AD0  ret 0x18 = 6 个栈参数
//   Nation::add_action            0x0041B7C0  ret 0x28 = 10 个栈参数
//   Array::operator[]             0x0041C1D0  转发 thunk → 0x41C250；ECX=数组对象，返回元素指针
//   firm_array                    0x0069FB64  ※ .data 未初始化区，只能运行时校验
// ===========================================================================

constexpr uint32_t ADDR_FIND_BEST_PLACE_LOC = 0x00427AD0;
constexpr uint32_t ADDR_ADD_ACTION          = 0x0041B7C0;
constexpr uint32_t ADDR_ARRAY_AT            = 0x0041C1D0;
constexpr uint32_t ADDR_FIRM_ARRAY          = 0x0069FB64;

// Array 内部布局（由 0x41C250 反编译确认）
constexpr int ARR_COUNT_OFF     = 0x0C;
constexpr int ARR_ELEM_SIZE_OFF = 0x10;
constexpr int ARR_DATA_OFF      = 0x19;

// Firm/Place 坐标（与 think_military 里 bestTownPtr->loc_x1/x2 一致）
constexpr int OFF_FIRM_LOC_X = 0x23;
constexpr int OFF_FIRM_LOC_Y = 0x25;

constexpr int ACTION_AI_BUILD_FIRM = 1;
constexpr int FIRM_FORT_ID         = 2;

typedef void (*FnThinkBuildFirm)(void* self);
typedef int  (__thiscall* FnFindBestPlaceLoc)(void* self, int firmId, short x, short y, short* outX, short* outY, int flag);
typedef int  (__thiscall* FnAddAction)(void* self, short x, short y, short refX, short refY,
                                       short mode, short para, short a7, short a8, short a9, short a10);
typedef void* (__thiscall* FnArrayAt)(void* arr, int recno);

static FnThinkBuildFirm g_tbfOrig = nullptr;
static volatile LONG g_tbfFirst = 0;
static volatile LONG g_fortHit = 0;
static volatile LONG g_fortLogged = 0;
static volatile LONG g_fortBadArr = 0;
static DWORD g_lastFortTick[MAX_NATION_SLOTS] = { 0 };
constexpr DWORD FORT_COOLDOWN_MS = 90000;   // 90 秒，避免刷屏

// firm_array 是 .data 未初始化区，静态读不到 → 运行时用内部字段反证
static bool FirmArrayLooksValid()
{
    const uint8_t* a = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ADDR_FIRM_ARRAY));
    const int count = *reinterpret_cast<const int*>(a + ARR_COUNT_OFF);
    const int elemSize = *reinterpret_cast<const int*>(a + ARR_ELEM_SIZE_OFF);
    const uint32_t dataPtr = *reinterpret_cast<const uint32_t*>(a + ARR_DATA_OFF);
    if (count < 1 || count > 500) return false;
    if (elemSize < 64 || elemSize > 4096) return false;
    if (dataPtr < 0x10000) return false;
    return true;
}

static void HookThinkBuildFirm(void* self)
{
    // 首次被调用时把 firm_array 的真实字段打出来（这是本补丁唯一的运行时未知量）
    if (InterlockedIncrement(&g_tbfFirst) == 1) {
        const uint8_t* a = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ADDR_FIRM_ARRAY));
        Log("[AI] think_build_firm 首次调用；firm_array(+0x%X) count=%d elemSize=%d data=0x%08X → 校验%s\r\n",
            ADDR_FIRM_ARRAY,
            *reinterpret_cast<const int*>(a + ARR_COUNT_OFF),
            *reinterpret_cast<const int*>(a + ARR_ELEM_SIZE_OFF),
            *reinterpret_cast<const uint32_t*>(a + ARR_DATA_OFF),
            FirmArrayLooksValid() ? "通过" : "不通过");
    }

    if (g_tbfOrig)
        g_tbfOrig(self);                        // 原有造矿逻辑照跑

    const uint8_t* n = static_cast<const uint8_t*>(self);
    const signed char raceId = *reinterpret_cast<const signed char*>(n + OFF_RACE_ID);
    if (raceId >= 0) return;                    // 人族：零影响
    if (-raceId == MONSTER_MINOTAUROS) return;  // 牛头人：经济先不动

    const short townCount = *reinterpret_cast<const short*>(n + OFF_AI_TOWN_COUNT);
    if (townCount > 0) return;                  // 有城镇 → 交给 think_military（补丁 2a）管

    const short campCount = *reinterpret_cast<const short*>(n + OFF_AI_CAMP_COUNT);
    if (campCount <= 0) return;                 // 连巢穴都没有 → 无从落脚

    const float cash = *reinterpret_cast<const float*>(n + OFF_CASH);
    if (cash < FORT_SETUP_COST) return;

    const short nationRecno = *reinterpret_cast<const short*>(n + OFF_NATION_RECNO);
    if (nationRecno <= 0 || nationRecno >= MAX_NATION_SLOTS) return;
    const DWORD now = GetTickCount();
    if (g_lastFortTick[nationRecno] != 0 && now - g_lastFortTick[nationRecno] < FORT_COOLDOWN_MS) return;

    // ---- 取巢穴坐标 ----
    if (!FirmArrayLooksValid()) {
        LONG b = InterlockedIncrement(&g_fortBadArr);
        if (b <= 3)
            Log("[X] firm_array 0x%08X 字段不合理，跳过巢穴旁造堡垒（第 %ld 次）\r\n",
                ADDR_FIRM_ARRAY, b);
        return;
    }
    const short* campArr = *reinterpret_cast<const short* const*>(n + OFF_AI_CAMP_ARRAY);
    if (!campArr) return;
    const int campRecno = campArr[0];
    if (campRecno <= 0) return;

    void* lair = ((FnArrayAt)static_cast<uintptr_t>(ADDR_ARRAY_AT))(
                     reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_FIRM_ARRAY)), campRecno);
    if (!lair) return;

    const short lx = *reinterpret_cast<const short*>(static_cast<const uint8_t*>(lair) + OFF_FIRM_LOC_X);
    const short ly = *reinterpret_cast<const short*>(static_cast<const uint8_t*>(lair) + OFF_FIRM_LOC_Y);

    // ---- 找地 + 下命令 ----
    short bx = 0, by = 0;
    if (!((FnFindBestPlaceLoc)static_cast<uintptr_t>(ADDR_FIND_BEST_PLACE_LOC))(
            self, FIRM_FORT_ID, lx, ly, &bx, &by, 0)) {
        return;
    }

    ((FnAddAction)static_cast<uintptr_t>(ADDR_ADD_ACTION))(
        self, bx, by, lx, ly,
        ACTION_AI_BUILD_FIRM, FIRM_FORT_ID, 1, 0, 0, 0);

    g_lastFortTick[nationRecno] = now;
    LONG hit = InterlockedIncrement(&g_fortHit);
    LONG lg  = InterlockedIncrement(&g_fortLogged);
    if (lg <= 20) {
        Log("[AI] 巢穴旁造堡垒 #%ld: monster=%d 巢穴#%d @(%d,%d) → 堡垒@(%d,%d) 现金=%.0f\r\n",
            hit, -raceId, campRecno, (int)lx, (int)ly, (int)bx, (int)by, cash);
    }
}

// ===========================================================================
// 诊断钩子：Nation::process_ai (0x00425C70)
//
// 目的：每个国家**每天**都会走这里，比"造矿"（每 15~300 天才一次）触发频率高几个数量级，
//       开局几秒就能把 Nation 的真实 vtable 打出来 ⇒ 据此取 [vtable+槽位] 定位外交函数。
//
// 序言 55 8B EC 83 EC 0C 56 89 4D FC（10 字节，4 条完整指令）→ stealSize = 10
// ===========================================================================

constexpr uint32_t RVA_PROCESS_AI = 0x25C70;   // VA 0x00425C70

typedef void (__fastcall* FnProcessAi)(void* self, void* edx);
static FnProcessAi g_paiOrig = nullptr;

// 前置声明：E 刀的中立巢穴每日检查（定义在文件后段）
static void NeutralLairTick();

// ★ 必须是 __fastcall：(ECX=this, EDX=x) 才能对上 __thiscall 的 Nation::process_ai。
//   2026-10-17 曾误写成普通函数（__cdecl）⇒ self 取到栈上垃圾，
//   读出的"vtable"其实是别的数据（含 "RESE_COM" 字符串），且可能拿垃圾指针调原函数。
static void __fastcall HookProcessAi(void* self, void* /*edx*/)
{
    if (LooksLikeNation(self))          // ★ 身份校验：坏指针连 self 都不读
        LogNationVtableOnce(self);

    NeutralLairTick();                  // ★ E 刀：中立巢穴每日（30 秒节流）检查

    if (g_paiOrig)
        g_paiOrig(self, nullptr);
}

bool InstallProcessAiProbe()
{
    void* target = Rva(RVA_PROCESS_AI);
    static const uint8_t HEAD[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x56,0x89,0x4D,0xFC };
    if (!InImageRange(target, sizeof(HEAD)) || memcmp(target, HEAD, sizeof(HEAD)) != 0) {
        Log("[X] process_ai 字节校验失败，跳过 vtable 探针\r\n");
        return false;
    }
    Log("[i] process_ai @ 00425C70（字节校验）\r\n");

    g_paiOrig = reinterpret_cast<FnProcessAi>(BuildTrampoline(target, 10));
    if (!g_paiOrig) {
        Log("[X] process_ai 跳板创建失败\r\n");
        return false;
    }

    uint8_t saved[5] = { 0 };
    if (!InstallJump(target, reinterpret_cast<void*>(&HookProcessAi), saved)) {
        Log("[X] process_ai 安装 JMP 失败\r\n");
        g_paiOrig = nullptr;
        return false;
    }

    Log("[O] vtable 探针已安装：进游戏后日志会出现一行 [VT] ...\r\n");
    return true;
}

// ===========================================================================
// 叛乱者 AI 改造（B + C + D）
//
// 原版缺陷（源码 orebel.cpp）：
//   ① think_new_action() 就是 misc.random(3)，不看兵力、不看对手守军
//   ② 原本的 case 0 think_settle_new() 被注释 ⇒ 永远不会自己建城
//   ③ think_cur_action() 实质判断被注释 ⇒ 一旦停下就 IDLE，然后重新掷骰子
//
// 本补丁：hook Rebel::think_new_action，按**兵力分级**重写选路：
//   弱（≤2 人）：只建城 / 投奔，绝不攻城         ← 消灭"自杀式攻城"
//   中（3~7 人）：可打建筑，但不攻城镇
//   强（≥8 人）：保留原版三选一（含攻城）
// 并且每一档都带"失败回退链" ⇒ 几乎总能选出一个动作 ⇒ 不再发呆（B）
// 建新城（D）通过把 think_settle_new 加回候选实现。
//
// ---- 地址（静态 ImageBase 0x400000 视角；调用时统一走 G() 做重定位）----
//   Rebel::next_day                  0x00563170   （调用者，仅参考）
//   Rebel::think_new_action          0x00563390   ★ hook 目标
//   Rebel::think_cur_action          0x00563410
//   Rebel::think_town_action         0x005634B0
//   Rebel::think_settle_new          0x005634F0   ★ D 刀
//   Rebel::think_settle_to           0x00563610
//   Rebel::think_capture_attack_town 0x00563790
//   Rebel::think_attack_firm         0x00563930
//   Rebel::execute_new_action        0x00563AC0
//   unit_array                       0x0069CAB0   is_deleted / operator[]
//   unit_array.size() 用             0x0069CAB4
//   UnitArray::is_deleted            0x005F81A0
//   misc                             0x0069F4A0
//   misc.random                      0x00530E30
//
// ---- Rebel 对象布局（反编译确认）----
//   +0x0 short rebel_recno   +0x2 short leader_unit_recno  +0x4 char action_mode
//   +0x5 short action_para   +0x7 short action_para2       +0x9 short mobile_rebel_count
//   +0xB short town_recno
// ===========================================================================

constexpr uint32_t VA_REBEL_THINK_NEW_ACTION   = 0x00563390;
constexpr uint32_t VA_REBEL_THINK_SETTLE_NEW   = 0x005634F0;
constexpr uint32_t VA_REBEL_THINK_SETTLE_TO    = 0x00563610;
constexpr uint32_t VA_REBEL_THINK_ATK_TOWN     = 0x00563790;
constexpr uint32_t VA_REBEL_THINK_ATK_FIRM     = 0x00563930;
constexpr uint32_t VA_REBEL_EXECUTE_NEW_ACTION = 0x00563AC0;
constexpr uint32_t VA_UNIT_ARRAY               = 0x0069CAB0;
constexpr uint32_t VA_UNIT_IS_DELETED          = 0x005F81A0;
constexpr uint32_t VA_MISC                     = 0x0069F4A0;
constexpr uint32_t VA_MISC_RANDOM              = 0x00530E30;

constexpr int ROFF_LEADER = 0x02;
constexpr int ROFF_MOBILE = 0x09;

// ---- Unit / Skill 布局（反编译 + 头文件双重确认）----
//   Unit:  +0x7B Skill skill  （pack(1)，15 字节：potential1+2+2+2+4+4）
//          +0x80 Skill::std_hit_points (short)  ← 「战斗等级 100 时的血量」
//          +0x8A char  unit_mode
//          +0x8B short unit_mode_para
constexpr int UOFF_SKILL_STD_HP = 0x80;
constexpr int UOFF_UNIT_MODE    = 0x8A;
constexpr int UOFF_UNIT_PARA    = 0x8B;
constexpr int UNIT_MODE_REBEL_V = 6;

constexpr uint32_t VA_UNIT_ARRAY_AT   = 0x00584210;   // unit_array[i]
constexpr uint32_t VA_UNIT_ARRAY_SIZE = 0x0041C310;   // unit_array.size()（ECX=0x0069CAB4）

// 兵力分级阈值（可直接调）
constexpr short REBEL_WEAK_MAX = 2;    // ≤2 人：不攻城
constexpr short REBEL_MID_MAX  = 7;    // 3~7 人：打建筑，不攻城镇；≥8 人：全放开

typedef int  (__thiscall* FnRebelThink)(void* self);
typedef int  (__thiscall* FnArrayIsDeleted)(void* arr, int recno);
typedef int  (__thiscall* FnMiscRandom)(void* misc, int n);

static FnRebelThink g_rebelOrig = nullptr;
static volatile LONG g_rebelHits = 0;
static volatile LONG g_rebelLogged = 0;



static inline int RebelRand(int n)
{
    return reinterpret_cast<FnMiscRandom>(G(VA_MISC_RANDOM))(G(VA_MISC), n);
}

static inline int UnitIsDeleted(int recno)
{
    return reinterpret_cast<FnArrayIsDeleted>(G(VA_UNIT_IS_DELETED))(G(VA_UNIT_ARRAY), recno);
}

static inline int RebelCallThink(uint32_t fnVA, void* self)
{
    return reinterpret_cast<FnRebelThink>(G(fnVA))(self);
}

// ---------------------------------------------------------------------------
// 叛军生命值 ×2
//
// 思路：不动内联的 max_hit_points()，而是把每个叛军单位的
//       Skill::std_hit_points（Unit+0x80, short）翻一倍 ——
//       引擎之后每次计算最大血量都会自动跟着翻倍。
//
// 幂等保护：用"原始值表"记住每个单位翻倍前的值；
//           若当前值既不是原值也不是原值的 2 倍 ⇒ 判定为复用了 recno 的新单位，重新记录。
// ---------------------------------------------------------------------------
static short g_hpOrig[65536] = { 0 };   // 按 unit recno 低 16 位索引
static volatile LONG g_hpBoosted = 0;
static volatile LONG g_hpSkip = 0;

typedef void* (__thiscall* FnUnitArrayAt)(void* arr, int recno);
typedef int   (__thiscall* FnUnitArraySize)(void* arrPlus4);

static void BoostRebelHp(void* rebel)
{
    if (!ReadIntSwitch("airephp", 1))
        return;

    const short rebelRecno = *reinterpret_cast<const short*>(static_cast<const char*>(rebel) + 0x00);
    if (rebelRecno <= 0)
        return;

    void* arrObj  = G(VA_UNIT_ARRAY);
    void* arrObj4 = G(VA_UNIT_ARRAY + 4);

    int count = reinterpret_cast<FnUnitArraySize>(G(VA_UNIT_ARRAY_SIZE))(arrObj4);
    if (count <= 0 || count > 20000)
        return;

    for (int i = count; i >= 1; --i) {
        if (UnitIsDeleted(i))
            continue;

        void* u = reinterpret_cast<FnUnitArrayAt>(G(VA_UNIT_ARRAY_AT))(arrObj, i);
        if (!u || reinterpret_cast<uintptr_t>(u) < 0x10000)
            continue;

        const char  mode = *reinterpret_cast<const char*>(static_cast<const char*>(u) + UOFF_UNIT_MODE);
        const short para = *reinterpret_cast<const short*>(static_cast<const char*>(u) + UOFF_UNIT_PARA);
        if (mode != UNIT_MODE_REBEL_V || para != rebelRecno)
            continue;

        short* stdHp = reinterpret_cast<short*>(static_cast<char*>(u) + UOFF_SKILL_STD_HP);
        const short cur = *stdHp;
        if (cur <= 0 || cur >= 30000)
            continue;

        short& orig = g_hpOrig[i & 0xFFFF];

        // 已经翻倍过（当前值 == 记录的原值 × 2）⇒ 跳过
        if (orig != 0 && cur == static_cast<short>(orig * 2)) {
            InterlockedIncrement(&g_hpSkip);
            continue;
        }

        // 新单位 / recno 被复用 / 首次见到 ⇒ 记录原值再翻倍
        orig = cur;
        const short doubled = static_cast<short>(cur * 2);
        *stdHp = doubled;

        LONG n = InterlockedIncrement(&g_hpBoosted);
        if (n <= 10)
            Log("[AI] 叛军生命值 x2: unit#%d stdHP %d -> %d\r\n", i, (int)cur, (int)doubled);
    }
}

static void __fastcall HookRebelThinkNewAction(void* self, void* /*edx*/)
{
    // 先自检：self 得像个 Rebel（town_recno 和 mobile_rebel_count 不能离谱）
    if (!self) return;
    if (reinterpret_cast<uintptr_t>(self) < 0x10000) return;

    const short leader = *reinterpret_cast<const short*>(static_cast<const char*>(self) + ROFF_LEADER);
    if (leader <= 0) return;
    if (UnitIsDeleted(leader))     // 原版第一行的守卫，必须照做（否则子函数会空指针）
        return;

    const short mobile = *reinterpret_cast<const short*>(static_cast<const char*>(self) + ROFF_MOBILE);
    const short town   = *reinterpret_cast<const short*>(static_cast<const char*>(self) + 0x0B);

    // 叛军生命值 ×2（幂等：只翻一次）
    BoostRebelHp(self);

    int rc = 0;
    const int dice = RebelRand(100);

    if (mobile <= REBEL_WEAK_MAX) {
        // ---- 弱：只求自保，绝不攻城 ----
        if (dice < 45) rc = RebelCallThink(VA_REBEL_THINK_SETTLE_NEW, self);
        if (!rc)       rc = RebelCallThink(VA_REBEL_THINK_SETTLE_TO,  self);
        if (!rc)       rc = RebelCallThink(VA_REBEL_THINK_SETTLE_NEW, self);
    }
    else if (mobile <= REBEL_MID_MAX) {
        // ---- 中：可以打建筑，但不碰城镇 ----
        if      (dice < 35) rc = RebelCallThink(VA_REBEL_THINK_SETTLE_TO,  self);
        else if (dice < 70) rc = RebelCallThink(VA_REBEL_THINK_ATK_FIRM,   self);
        else                rc = RebelCallThink(VA_REBEL_THINK_SETTLE_NEW, self);
        if (!rc) rc = RebelCallThink(VA_REBEL_THINK_ATK_FIRM,   self);
        if (!rc) rc = RebelCallThink(VA_REBEL_THINK_SETTLE_TO,  self);
        if (!rc) rc = RebelCallThink(VA_REBEL_THINK_SETTLE_NEW, self);
    }
    else {
        // ---- 强：交回原版（含攻城）----
        if (g_rebelOrig)
            rc = g_rebelOrig(self);
    }

    if (rc) {
        reinterpret_cast<FnRebelThink>(G(VA_REBEL_EXECUTE_NEW_ACTION))(self);

        LONG lg = InterlockedIncrement(&g_rebelLogged);
        if (lg <= 30) {
            Log("[AI] 叛乱者新动作 #%ld: 兵力=%d 城池=%d 骰子=%d 分级=%s\r\n",
                lg, (int)mobile, (int)town, dice,
                mobile <= REBEL_WEAK_MAX ? "弱(只自保)" :
                mobile <= REBEL_MID_MAX  ? "中(打建筑)" : "强(全放开)");
        }
    } else {
        InterlockedIncrement(&g_rebelHits);
    }
}

// ===========================================================================
// E 刀 第 0 段：中立巢穴只读探针（零引擎调用）
//
// 依据：Array::operator[] (0x41C250) 的实现就是一句指针算术 ——
//     if (i < 1 || count < i) return 0;
//     return *(void**)( (i-1)*elem_size + data );
//   0x584210 只是它的转发 thunk ⇒ 我自己算一遍即可，**不调用任何引擎函数**。
//
// Array 字段（0x41C250 反编译确认）：
//   count @ +0x0C    elem_size @ +0x10    data @ +0x19（未对齐）
//
// 三段式开关：ailair=0 全关 / 1 只读探针 / 2 假 Rebel 移动（未实现）
// ===========================================================================

constexpr uint32_t VA_FIRM_ARRAY_0 = 0x0069FB64;   // 实测 count=86 ✅
constexpr uint32_t VA_TOWN_ARRAY_0 = 0x00688700;   // 实测 count=42 ✅
constexpr uint32_t VA_UNIT_ARRAY_0 = 0x0069CAB4;   // ★ 修正：0x69CAB0 是错的（会读到 count=1/data=0x00FFFFFF）   // 待验（实测读到垃圾）

constexpr int OOFF_NATION_RECNO = 0x0B;
constexpr int OOFF_LOC_X        = 0x23;
constexpr int OOFF_LOC_Y        = 0x25;
constexpr int FOFF_FIRM_ID      = 0x42;
constexpr char  UNIT_MODE_MONSTER_V = 7;
constexpr short FIRM_LAIR_ID    = 12;
constexpr short LAIR_THRESHOLD  = 12;

// 合法的用户态指针范围：低于 16MB 基本可判为垃圾/哨兵
constexpr uintptr_t PTR_MIN = 0x01000000;
constexpr uintptr_t PTR_MAX = 0x7FF00000;

struct ArrView0 { int count; int elemSize; uint32_t data; bool ok; };

static ArrView0 ViewArray0(uint32_t staticVA)
{
    ArrView0 v = { 0, 0, 0, false };
    uintptr_t a = reinterpret_cast<uintptr_t>(G(staticVA));
    if (a < 0x10000)
        return v;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
    v.count    = *reinterpret_cast<const int*>(p + 0x0C);
    v.elemSize = *reinterpret_cast<const int*>(p + 0x10);
    v.data     = *reinterpret_cast<const uint32_t*>(p + 0x19);
    v.ok = (v.count > 0 && v.count < 20000)
        && (v.elemSize >= 4 && v.elemSize <= 4096)
        && (v.data >= PTR_MIN && v.data < PTR_MAX);
    return v;
}

static void* ArrElem0(const ArrView0& v, int recno)
{
    if (!v.ok || recno < 1 || recno > v.count)
        return nullptr;
    uintptr_t addr = v.data + static_cast<uintptr_t>(recno - 1) * v.elemSize;
    if (addr < PTR_MIN || addr > PTR_MAX)      // ★ 硬保护：绝不 deref 垃圾指针
        return nullptr;
    return *reinterpret_cast<void* const*>(addr);
}

// 把某个地址周围 16 个 dword 打出来，用于人工判读布局
static void DumpDwords(const char* tag, uint32_t staticVA)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(G(staticVA));
    if (a < 0x10000)
        return;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(a & ~3u);
    Log("[AI]   %s @0x%08X:\r\n   ", tag, staticVA);
    for (int i = 0; i < 16; ++i)
        Log(" %08X", p[i]);
    Log("\r\n");
}

// ---- 第 1 段：调用 FirmCamp::patrol(0) 让守军出巢 ----
//   0x00478160 = FirmCamp::patrol(short patrolType)，__thiscall
//   源码证实它只读 nation_array.player_recno(0x69DBB2)，不碰 nation_array[nation_recno]
//   ⇒ 中立巢穴可安全调用。patrolType=0 ⇒ 监工+全部士兵一起上马。
constexpr uint32_t VA_PATROL = 0x00478160;
static const uint8_t PATROL_SIG[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
typedef void (__thiscall* FnPatrol)(void* self, short patrolType);

// ---- 出巢后立刻给归宿（ailair=3）----
constexpr uint32_t VA_UNIT_GROUP_0 = 0x0069CAE4;
constexpr uint32_t VA_UG_ZAP0      = 0x00469F70;
constexpr uint32_t VA_UG_ADD0      = 0x005E6830;
constexpr uint32_t VA_UG_ASSIGN0   = 0x005E6CE0;
constexpr short UNIT_LAND_0 = 0x4C;
constexpr short COMMAND_AI_0 = 2;

typedef void (__fastcall* FnUgZap0)(void* g, void* edx, int a);
typedef void (__fastcall* FnUgAdd0)(void* g, void* edx, int recno);
typedef void (__fastcall* FnUgAttack0)(void* g, void* edx, int targetRecno, int cmd);
constexpr uint32_t VA_UG_ATTACK0 = 0x005E6B50;   // UnitGroup::attack(recno, COMMAND_AI)
typedef void (__fastcall* FnUgAssign0)(void* g, void* edx, short x, short y,
                                       short land, short cmd, int a5, int a6);

constexpr DWORD LAIR_OUT_MS = 180000;          // 同一巢穴 3 分钟内不重复触发
static DWORD g_lairOutAt2[512] = { 0 };

// ---- ailair=4：定时出巢兜圈 ----
struct LairPatrolState { DWORD nextTick; short left; short phase; };
static LairPatrolState g_patrol4[512] = { { 0, 0, 0 } };
constexpr DWORD PATROL_STEP_MS = 18000;   // 每 18 秒换一个航点
constexpr short PATROL_WAYPOINTS = 4;     // 绕 4 个点

static DWORD g_lairTick0 = 0;

static void NeutralLairTick()
{
    const int mode = ReadIntSwitch("ailair", 0);
    if (mode <= 0)
        return;

    const DWORD now = GetTickCount();
    if (g_lairTick0 != 0 && now - g_lairTick0 < 30000)   // 30 秒节流
        return;
    g_lairTick0 = now;

    ArrView0 uA = ViewArray0(VA_UNIT_ARRAY_0);
    ArrView0 fA = ViewArray0(VA_FIRM_ARRAY_0);
    ArrView0 tA = ViewArray0(VA_TOWN_ARRAY_0);

    Log("[AI] 巢穴探针: unit(%s c=%d e=%d d=0x%08X) | firm(%s c=%d e=%d d=0x%08X) | town(%s c=%d)\r\n",
        uA.ok ? "OK" : "BAD", uA.count, uA.elemSize, uA.data,
        fA.ok ? "OK" : "BAD", fA.count, fA.elemSize, fA.data,
        tA.ok ? "OK" : "BAD", tA.count);

    // ---- 原始内存对照：已知正确的 firm_array vs 待验的 unit_array ----
    DumpDwords("firm_array(已验)", VA_FIRM_ARRAY_0);
    DumpDwords("unit_array(待验)", VA_UNIT_ARRAY_0);

    if (!uA.ok || !fA.ok) {
        Log("[AI]   数组校验未通过，本轮不做统计\r\n");
        return;
    }

    // ---- 统计每个巢穴里有多少魔族（纯读）----
    static short cnt[512];
    for (int i = 0; i < 512; ++i)
        cnt[i] = 0;

    for (int i = uA.count; i >= 1; --i) {
        void* u = ArrElem0(uA, i);
        if (!u)
            continue;
        const uint8_t* up = reinterpret_cast<const uint8_t*>(u);
        if (up[UOFF_UNIT_MODE] != UNIT_MODE_MONSTER_V)
            continue;
        const short para = *reinterpret_cast<const short*>(up + UOFF_UNIT_PARA);
        if (para > 0)
            cnt[para & 0x1FF]++;
    }

    int neutralLairs = 0, readyLairs = 0;
    for (int i = fA.count; i >= 1; --i) {
        void* f = ArrElem0(fA, i);
        if (!f)
            continue;
        const uint8_t* fp = reinterpret_cast<const uint8_t*>(f);
        if (fp[OOFF_NATION_RECNO] != 0)
            continue;
        if (fp[FOFF_FIRM_ID] != FIRM_LAIR_ID)
            continue;
        ++neutralLairs;
        // ★ 巢穴里的兵数 = FirmCamp::soldier_count（Firm+0x3CD，char）
        const int n = static_cast<int>(static_cast<unsigned char>(fp[0x3CD]));
        const short overseer = *reinterpret_cast<const short*>(fp + 0x128);
        const short lx = *reinterpret_cast<const short*>(fp + OOFF_LOC_X);
        const short ly = *reinterpret_cast<const short*>(fp + OOFF_LOC_Y);

        if (mode < 4 && n < LAIR_THRESHOLD) {
            Log("[AI]   巢穴#%d 兵=%d 监工=%d（未达 %d）\r\n", i, n, (int)overseer, (int)LAIR_THRESHOLD);
            continue;
        }
        ++readyLairs;

        const int slot = i & 0x1FF;

        // ---- ailair=4：巡逻中的巢穴 —— 到点就换下一个航点，永不给"空指令" ----
        if (g_patrol4[slot].phase == 1) {
            if (now < g_patrol4[slot].nextTick)
                continue;
            static const short WX[4] = { 7, 0, -7, 0 };
            static const short WY[4] = { 0, 7, 0, -7 };
            short wx, wy;
            if (g_patrol4[slot].left > 0) {
                const int k2 = (PATROL_WAYPOINTS - g_patrol4[slot].left) % 4;
                wx = static_cast<short>(lx + WX[k2]);
                wy = static_cast<short>(ly + WY[k2]);
                g_patrol4[slot].left--;
                g_patrol4[slot].nextTick = now + PATROL_STEP_MS;
            } else {
                wx = lx;  wy = ly;               // 回巢
                g_patrol4[slot].phase = 0;
            }
            // 重新编组：patrol_unit_array 里一直存着这批单位的 recno
            void* ug2 = G(VA_UNIT_GROUP_0);
            reinterpret_cast<FnUgZap0>(G(VA_UG_ZAP0))(ug2, nullptr, 1);
            const int pc2 = static_cast<int>(static_cast<unsigned char>(fp[0x3D1]));
            int back = 0;
            for (int k = 0; k < pc2 && k < 16; ++k) {
                const short rc = *reinterpret_cast<const short*>(fp + 0x3D2 + k * 2);
                if (rc <= 0) continue;
                reinterpret_cast<FnUgAdd0>(G(VA_UG_ADD0))(ug2, nullptr, rc & 0xFFFF);
                ++back;
            }
            if (back > 0) {
                reinterpret_cast<FnUgAssign0>(G(VA_UG_ASSIGN0))(ug2, nullptr,
                    wx, wy, UNIT_LAND_0, COMMAND_AI_0, -1, 0);
                Log("[AI]     巢穴#%d 巡逻中：%d 个单位 → (%d,%d)，剩 %d 个航点\r\n",
                    i, back, (int)wx, (int)wy, (int)g_patrol4[slot].left);
            }
            continue;
        }

        if (n < LAIR_THRESHOLD)
            continue;

        Log("[AI]   中立巢穴#%d @(%d,%d) 兵=%d 监工=%d ⇒ 够格出动\r\n", i, (int)lx, (int)ly, n, (int)overseer);

        if (mode < 2)
            continue;
        if (overseer == 0) {
            Log("[AI]     跳过：没有监工\r\n");
            continue;
        }
        if (g_lairOutAt2[slot] != 0 && now - g_lairOutAt2[slot] < LAIR_OUT_MS) {
            Log("[AI]     冷却中（%lu 秒前刚出动过）\r\n",
                (unsigned long)((now - g_lairOutAt2[slot]) / 1000));
            continue;
        }

        // ---- 字节校验 ----
        const uint8_t* pp = reinterpret_cast<const uint8_t*>(G(VA_PATROL));
        bool sigOk = true;
        for (int z = 0; z < 6; ++z) {
            if (pp[z] != PATROL_SIG[z]) { sigOk = false; break; }
        }
        if (!sigOk) {
            Log("[X] patrol(0x%08X) 字节校验失败 ⇒ 本段不执行\r\n", VA_PATROL);
            return;
        }

        // ---- 出巢 ----
        reinterpret_cast<FnPatrol>(G(VA_PATROL))(f, 0);
        g_lairOutAt2[slot] = now;

        void* ug = G(VA_UNIT_GROUP_0);
        reinterpret_cast<FnUgZap0>(G(VA_UG_ZAP0))(ug, nullptr, 1);
        const int pcnt = static_cast<int>(static_cast<unsigned char>(fp[0x3D1]));
        int added = 0;
        for (int k = 0; k < pcnt && k < 16; ++k) {
            const short recno = *reinterpret_cast<const short*>(fp + 0x3D2 + k * 2);
            if (recno <= 0) continue;
            reinterpret_cast<FnUgAdd0>(G(VA_UG_ADD0))(ug, nullptr, recno & 0xFFFF);
            ++added;
        }

        if (added <= 0) {
            Log("[AI]     ⚠ 一个单位都没找到\r\n");
            continue;
        }

        // 第一个航点：朝"最近的人类城镇/魔族巢穴"方向走出第一步
        static const short WX0[4] = { 7, 0, -7, 0 };
        static const short WY0[4] = { 0, 7, 0, -7 };
        const short fx = static_cast<short>(lx + WX0[0]);
        const short fy = static_cast<short>(ly + WY0[0]);
        reinterpret_cast<FnUgAssign0>(G(VA_UG_ASSIGN0))(ug, nullptr,
            fx, fy, UNIT_LAND_0, COMMAND_AI_0, -1, 0);

        g_patrol4[slot].phase = 1;
        g_patrol4[slot].left = PATROL_WAYPOINTS - 1;
        g_patrol4[slot].nextTick = now + PATROL_STEP_MS;
        Log("[AI]     ★★ 出巢兜圈开始：编入 %d 个单位 → (%d,%d)，共绕 %d 个航点\r\n",
            added, (int)fx, (int)fy, (int)PATROL_WAYPOINTS);
    }
    Log("[AI]   中立巢穴总数=%d，够格=%d（阈值 %d 只）\r\n",
        neutralLairs, readyLairs, (int)LAIR_THRESHOLD);
}

// ===========================================================================
// 清剿叛军的代价：声誉 -0.3 / 人不掉忠诚
//
// 原版链路（onationb.cpp:1867）：
//   NationBase::civilian_killed(race, isAttacker, killedCount)
//     → change_all_people_loyalty(-2 * N, race)   ★ 全民族忠诚 -2/人（死亡螺旋主因）
//     → change_reputation(-1 * N)                 声誉 -1/人
//   而叛乱者被当作"平民"结算 ⇒ 每杀一个叛军触发上面两条。
//
// 改法：两处 hook
//   ① Unit::update_being_killed_statistic 0x005E3440：死者是叛军 ⇒ 置全局标记再跑原函数
//   ② NationBase::civilian_killed        0x00540AF0：看到标记且 isAttacker ⇒
//        只调 change_reputation(-0.3 * N)，**跳过原函数**（忠诚完全不动）
//
// 地址（均经 REA 反编译逐行比对源码确认）：
//   Unit::update_being_killed_statistic  0x005E3440  序言 55 8B EC 83 EC 18 89 4D F8（9 字节）
//   NationBase::civilian_killed          0x00540AF0  序言 55 8B EC 51 89 4D FC（7 字节）
//   NationBase::change_reputation        0x0053FDB0  （只调用，不 hook）
//   NationBase::change_all_people_loyalty 0x00540B60 （不碰）
// ===========================================================================

constexpr uint32_t VA_UPDATE_BEING_KILLED = 0x005E3440;
constexpr uint32_t VA_CIVILIAN_KILLED     = 0x00540AF0;
constexpr uint32_t VA_CHANGE_REPUTATION   = 0x0053FDB0;

constexpr float REBEL_KILL_REPUTATION = -0.3f;   // 每杀一个叛军的声誉代价（原版 -1.0）

typedef void (__fastcall* FnUpdateKilled)(void* self, void* edx, void* attacker);
typedef void (__fastcall* FnCivilianKilled)(void* self, void* edx, int race, int isAttacker, int killedCount);
typedef void (__fastcall* FnChangeReputation)(void* self, void* edx, float amount);

static FnUpdateKilled   g_ukOrig = nullptr;
static FnCivilianKilled g_ckOrig = nullptr;
static volatile LONG g_inRebelKill = 0;       // 1 = 当前这次死亡是叛军
static volatile LONG g_rebelKillLogged = 0;

static void __fastcall HookUpdateBeingKilled(void* self, void* /*edx*/, void* attacker)
{
    if (self && reinterpret_cast<uintptr_t>(self) > 0x10000) {
        const char mode = *reinterpret_cast<const char*>(static_cast<const char*>(self) + UOFF_UNIT_MODE);
        if (mode == UNIT_MODE_REBEL_V) {
            InterlockedExchange(&g_inRebelKill, 1);
            if (g_ukOrig) g_ukOrig(self, nullptr, attacker);
            InterlockedExchange(&g_inRebelKill, 0);
            return;
        }
    }
    if (g_ukOrig) g_ukOrig(self, nullptr, attacker);
}

static void __fastcall HookCivilianKilled(void* self, void* /*edx*/, int race, int isAttacker, int killedCount)
{
    if (isAttacker && InterlockedCompareExchange(&g_inRebelKill, 0, 0) != 0) {
        // ---- 杀的是叛军：只掉一点声誉，忠诚完全不动 ----
        if (killedCount < 1) killedCount = 1;
        const float delta = REBEL_KILL_REPUTATION * static_cast<float>(killedCount);
        if (self) reinterpret_cast<FnChangeReputation>(G(VA_CHANGE_REPUTATION))(self, nullptr, delta);

        LONG n = InterlockedIncrement(&g_rebelKillLogged);
        if (n <= 20)
            Log("[AI] 叛军缩水惩罚: 声誉 %+.2f（%d 人，忠诚不变）\r\n", delta, killedCount);
        return;
    }
    if (g_ckOrig) g_ckOrig(self, nullptr, race, isAttacker, killedCount);
}

bool InstallRebelKillPenalty()
{
    if (!ReadAiSwitch()) {
        Log("[i] render.ini 里 ai=0，跳过叛军惩罚补丁\r\n");
        return false;
    }
    if (!ReadIntSwitch("airep", 1)) {
        Log("[i] airep=0：跳过叛军惩罚补丁\r\n");
        return false;
    }

    // ---- ① update_being_killed_statistic ----
    void* t1 = Rva(VA_UPDATE_BEING_KILLED - 0x400000u);
    static const uint8_t H1[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x18,0x89,0x4D,0xF8 };
    if (!InImageRange(t1, sizeof(H1)) || memcmp(t1, H1, sizeof(H1)) != 0) {
        Log("[X] update_being_killed_statistic 字节校验失败\r\n");
        return false;
    }
    g_ukOrig = reinterpret_cast<FnUpdateKilled>(BuildTrampoline(t1, 9));
    if (!g_ukOrig) { Log("[X] update_being_killed 跳板失败\r\n"); return false; }
    uint8_t saved[5] = { 0 };
    if (!InstallJump(t1, reinterpret_cast<void*>(&HookUpdateBeingKilled), saved)) {
        Log("[X] update_being_killed 安装失败\r\n"); g_ukOrig = nullptr; return false;
    }
    Log("[i] Unit::update_being_killed_statistic @ 005E3440（字节校验）\r\n");

    // ---- ② civilian_killed ----
    void* t2 = Rva(VA_CIVILIAN_KILLED - 0x400000u);
    static const uint8_t H2[] = { 0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC };
    if (!InImageRange(t2, sizeof(H2)) || memcmp(t2, H2, sizeof(H2)) != 0) {
        Log("[X] civilian_killed 字节校验失败\r\n");
        return false;
    }
    g_ckOrig = reinterpret_cast<FnCivilianKilled>(BuildTrampoline(t2, 7));
    if (!g_ckOrig) { Log("[X] civilian_killed 跳板失败\r\n"); return false; }
    if (!InstallJump(t2, reinterpret_cast<void*>(&HookCivilianKilled), saved)) {
        Log("[X] civilian_killed 安装失败\r\n"); g_ckOrig = nullptr; return false;
    }
    Log("[i] NationBase::civilian_killed @ 00540AF0（字节校验）\r\n");

    Log("[O] 叛军惩罚补丁已安装：杀叛军声誉仅 -0.3/人，忠诚不变\r\n");
    return true;
}

bool InstallRebelAI()
{
    if (!ReadAiSwitch()) {
        Log("[i] render.ini 里 ai=0，跳过叛乱者 AI 改造\r\n");
        return false;
    }
    if (!ReadIntSwitch("airebel", 1)) {
        Log("[i] airebel=0：跳过叛乱者 AI 改造\r\n");
        return false;
    }

    void* target = Rva(VA_REBEL_THINK_NEW_ACTION - 0x400000u);
    static const uint8_t HEAD[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x89,0x4D,0xFC };
    if (!InImageRange(target, sizeof(HEAD)) || memcmp(target, HEAD, sizeof(HEAD)) != 0) {
        Log("[X] Rebel::think_new_action 字节校验失败，跳过叛乱者改造\r\n");
        return false;
    }
    Log("[i] Rebel::think_new_action @ 00563390（字节校验）\r\n");

    g_rebelOrig = reinterpret_cast<FnRebelThink>(BuildTrampoline(target, 9));
    if (!g_rebelOrig) {
        Log("[X] think_new_action 跳板创建失败\r\n");
        return false;
    }

    uint8_t saved[5] = { 0 };
    if (!InstallJump(target, reinterpret_cast<void*>(&HookRebelThinkNewAction), saved)) {
        Log("[X] think_new_action 安装 JMP 失败\r\n");
        g_rebelOrig = nullptr;
        return false;
    }

    Log("[O] 叛乱者 AI 改造已安装：弱队不攻城 / 强队才攻城 / 可自己建新城\r\n");
    return true;
}

bool InstallMonsterFort()
{
    // ⚠️ 默认关闭（2026-10-17）：
    //   运行时校验发现 firm_array(0x69FB64) 三个字段全为 0 ⇒ 地址是错的，本补丁无法工作；
    //   且该轮内出现过 RVA 0x1EDDC（ai_should_build_mine 内部）的 0xC0000005 崩溃，
    //   高度怀疑与本 hook 的两层 trampoline 嵌套有关。
    //   在 firm_array 真地址确认之前，**默认不装**；调试时在 render.ini 写 aifort=1 才启用。
    if (!ReadIntSwitch("aifort", 0)) {
        Log("[i] aifort=0（默认）：跳过巢穴旁造堡垒补丁（firm_array 地址尚未确认）\r\n");
        return false;
    }
    if (!ReadAiSwitch()) {
        Log("[i] render.ini 里 ai=0，跳过巢穴旁造堡垒补丁\r\n");
        return false;
    }

    void* target = Rva(RVA_THINK_BUILD_FIRM);
    static const uint8_t HEAD[] = { 0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC };
    if (!InImageRange(target, sizeof(HEAD)) || memcmp(target, HEAD, sizeof(HEAD)) != 0) {
        Log("[X] think_build_firm 字节校验失败，放弃巢穴旁造堡垒\r\n");
        return false;
    }
    Log("[i] think_build_firm @ 0041E7A0（字节校验）\r\n");

    g_tbfOrig = reinterpret_cast<FnThinkBuildFirm>(BuildTrampoline(target, 7));
    if (!g_tbfOrig) {
        Log("[X] think_build_firm 跳板创建失败\r\n");
        return false;
    }

    uint8_t saved[5] = { 0 };
    if (!InstallJump(target, reinterpret_cast<void*>(&HookThinkBuildFirm), saved)) {
        Log("[X] think_build_firm 安装 JMP 失败\r\n");
        g_tbfOrig = nullptr;
        return false;
    }

    Log("[O] 巢穴旁造堡垒补丁已安装：无城镇、有巢穴的魔族会在巢穴旁造堡垒\r\n");
    return true;
}

}  // namespace sk2
