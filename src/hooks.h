// hooks.h — 各阶段挂钩实现
#pragma once
#include <cstdint>

namespace sk2 {

bool InstallTextWidth();   // Stage 2
bool InstallFontInfo();    // Stage 3
bool InstallMonsterMine();      // Stage 4  魔族开矿
bool InstallMonsterMilitary();  // Stage 4b 魔族扩军/堡垒
bool InstallMonsterFort();      // Stage 4c 巢穴旁造堡垒（默认关闭，aifort=1 开启）
bool InstallProcessAiProbe();   // Stage 4d vtable 探针（诊断用）
bool InstallRebelKillPenalty();  // Stage 5b 叛军惩罚（声誉缩水/不掉忠诚)
bool InstallRebelAI();          // Stage 5  叛乱者 AI 改造（B+C+D）
void MonsterMineStats();        // 退出时打印 AI 补丁统计
void SelfTest();           // 离线自测：不依赖游戏资源验证双字节宽度
long TextWidthCallCount(); // text_width 被调用的累计次数
int  MeasureText(int* font, const char* s);  // 按引擎语义测量串宽（优先用原函数）

}  // namespace sk2
