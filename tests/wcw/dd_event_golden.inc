// [wcw dd] Shared golden cases for the in-game Datadog line format (aki-roadmap#125).
//
// The same file, byte for byte, is rt64 tests/wcw/dd_event_golden.inc and N64ModernRuntime
// librecomp/tests/wcw/dd_event_golden.inc. rt64's metrics (ddTag, ddParseTags, ddGameFromExe, ddEventLine) and the
// runtime's fatal breadcrumb (recomp::dd::tag, parse_tags, game_from_exe, event_line) must both produce these exact
// strings, so their two copies of the format can't drift apart: change one, and its tests fail until the cases (and
// so the other copy) agree again. AkiTools tests/test_lib_sync.py checks that the two files stay identical.
//
// Each test file defines the four macros before including this one:
//   DD_GOLDEN_TAG(key, value, expected tag)
//   DD_GOLDEN_TAGS(WCW_DD_TAGS list, expected tags joined with ';', each followed by ';')
//   DD_GOLDEN_GAME(exe path or name, expected game)
//   DD_GOLDEN_EVENT(title, text, tags joined with ';', alert type, aggregation key, expected event line)

DD_GOLDEN_TAG("game", "nomercy", "game:nomercy")
DD_GOLDEN_TAG("Game", "No Mercy!", "game:no_mercy")
DD_GOLDEN_TAG("1st", "x", "t_1st:x")
DD_GOLDEN_TAG("owner", "Fire Ant", "owner:fire_ant")
DD_GOLDEN_TAG("k", "a|b,c#d", "k:a_b_c_d")
DD_GOLDEN_TAG("k", "  spaced  ", "k:spaced")
DD_GOLDEN_TAG("k", "", "")
DD_GOLDEN_TAG("", "v", "t_:v")
DD_GOLDEN_TAG("k", "__x__", "k:x")
DD_GOLDEN_TAG("con-ver.sion/x", "btbw:1", "con-ver.sion/x:btbw:1")
DD_GOLDEN_TAG("k", "\xC3\x84\xC3\x96", "")
DD_GOLDEN_TAG("_k_", "v", "k:v")

DD_GOLDEN_TAGS("conversion:btbw,owner:Fire Ant", "conversion:btbw;owner:fire_ant;")
DD_GOLDEN_TAGS("a:1,,b,c:,a:1,D:Two words", "a:1;d:two_words;")
DD_GOLDEN_TAGS("", "")
DD_GOLDEN_TAGS("x:y|z", "x:y_z;")
DD_GOLDEN_TAGS(":v,k:", "t_:v;")

DD_GOLDEN_GAME("NoMercyRecompiled.exe", "nomercy")
DD_GOLDEN_GAME("C:\\games\\Vpw2Recompiled.exe", "vpw2")
DD_GOLDEN_GAME("/x/WCWRecompiled.exe", "wt")
DD_GOLDEN_GAME("AkiLauncher", "akilauncher")
DD_GOLDEN_GAME("Foo.EXE", "foo")
DD_GOLDEN_GAME("", "")

DD_GOLDEN_EVENT("t", "x", "", "", "", "_e{1,1}:t|x|s:aki-game")
DD_GOLDEN_EVENT("title\nwith newline", "line1\nline2\r\n", "game:nm;kind:fatal", "error", "aki-fatal",
    "_e{18,14}:title with newline|line1\\nline2\\n|k:aki-fatal|s:aki-game|t:error|#game:nm,kind:fatal")
DD_GOLDEN_EVENT("\xC3\xA9", "\xC3\xBC", "a:b;", "info", "", "_e{2,2}:\xC3\xA9|\xC3\xBC|s:aki-game|t:info|#a:b")
