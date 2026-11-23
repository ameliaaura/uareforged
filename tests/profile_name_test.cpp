// Save screen profile names (src/utils.h): New must not throw on a profile named "UNNAMED" with no
// number, and "." / ".." are not folder names a profile may use. No SDL, no game data.
//   c++ -std=c++17 -Wall -Isrc tests/profile_name_test.cpp -o profile_name_test
#include "utils.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;

static void Check(bool ok, const char *what)
{
    g_checks++;
    if ( !ok )
    {
        g_failed++;
        printf("FAIL: %s\n", what);
    }
}

int main()
{
    Check(ProfileNumberSuffix("12") == 12, "UNNAMED12 -> 12");
    Check(ProfileNumberSuffix("0") == 0, "UNNAMED0 -> 0");
    Check(ProfileNumberSuffix("") == -1, "UNNAMED alone (crashed with stoi)");
    Check(ProfileNumberSuffix("x") == -1, "UNNAMEDx (crashed with stoi)");
    Check(ProfileNumberSuffix("3a") == -1, "UNNAMED3a");
    Check(ProfileNumberSuffix(" 3") == -1, "UNNAMED 3");
    Check(ProfileNumberSuffix("-3") == -1, "UNNAMED-3");
    Check(ProfileNumberSuffix("999999999") == 999999999, "9 digits");
    Check(ProfileNumberSuffix("99999999999") == -1, "too long for int (stoi out_of_range)");

    Check(!CheckValidUsername("."), "\".\" refused");
    Check(!CheckValidUsername(".."), "\"..\" refused");
    Check(CheckValidUsername("..."), "\"...\" is a plain name");
    Check(CheckValidUsername(".a"), "\".a\" is a plain name");
    Check(CheckValidUsername("Player"), "Player");

    printf("profile_name: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
