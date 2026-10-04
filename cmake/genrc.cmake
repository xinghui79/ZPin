# genrc —— 构建期把 assets/zpin.rc 模板展开成构建目录里的 zpin.rc：
# 把 @ZPIN_VERSION@ / @ZPIN_VERSION_CSV@ / @ZPIN_ICO@ 三个占位符换成实值。
# 用法（CMakeLists 的 zpin_rc 目标调用）：
#   cmake -DIN=<模板> -DOUT=<产物> -DVER=<版本> -DVERCSV=<逗号版本> -DICO=<ico路径> -P genrc.cmake
file(READ "${IN}" RC_TEXT)
string(REPLACE "@ZPIN_VERSION_CSV@" "${VERCSV}" RC_TEXT "${RC_TEXT}")
string(REPLACE "@ZPIN_VERSION@" "${VER}" RC_TEXT "${RC_TEXT}")
string(REPLACE "@ZPIN_ICO@" "${ICO}" RC_TEXT "${RC_TEXT}")
file(WRITE "${OUT}" "${RC_TEXT}")
