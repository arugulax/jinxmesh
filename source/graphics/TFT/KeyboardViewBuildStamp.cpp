// When this build was made, for the About page: "2026-11-23 18:27".
// The compiler fills in __DATE__ and __TIME__ when it compiles this file, so the build
// scripts touch it before every build. Nothing else belongs here.
#include <cstdio>
#include <cstring>
#include <string>

const char *keyboardViewBuildStamp()
{
    static char stamp[20];
    if (!stamp[0]) {
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char *date = __DATE__; // "Nov 23 2026", the day padded with a space
        const char *at = strstr(months, std::string(date, 3).c_str());
        int month = at ? (int)(at - months) / 3 + 1 : 0;
        int day = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0');
        snprintf(stamp, sizeof(stamp), "%.4s-%02d-%02d %.5s", date + 7, month, day, __TIME__);
    }
    return stamp;
}
