// 切换类型判定 + 停留截断的单测：不起 X、不起 dock，直接调生产代码里的纯函数
#include "../src/contextswitch.h"
#include <cstdio>
#include <cstring>

static int failed = 0;
static void expectKind(bool same, bool wx, const char *want)
{
    const char *got = SwitchRule::kindName(SwitchRule::kindFor(same, wx));
    if (strcmp(got, want) != 0) {
        printf("FAIL kind(same=%d, wx=%d) = %s, want %s\n", same, wx, got, want);
        ++failed;
    }
}
static void expectDwell(long long gap, int want)
{
    const int got = SwitchRule::clampDwellSec(gap);
    if (got != want) {
        printf("FAIL clampDwellSec(%lld) = %d, want %d\n", gap, got, want);
        ++failed;
    }
}

int main()
{
    expectKind(false, false, "window"); // 跨应用
    expectKind(false, true, "window");  // 进出企业微信也是跨应用
    expectKind(true, true, "chat");     // 企业微信内换会话
    expectKind(true, false, "inner");   // Chrome 换标签、文件管理器换目录
    expectDwell(0, 0);
    expectDwell(-5, 0);                 // 时钟回拨不能出负数
    expectDwell(90, 90);
    expectDwell(20 * 60, 20 * 60);
    expectDwell(64 * 3600, 20 * 60);    // 休眠 64 小时截到 20 分钟
    printf(failed ? "FAILED %d\n" : "ALL PASS\n", failed);
    return failed ? 1 : 0;
}
