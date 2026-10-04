# ZPin 编码规则（对所有 AI 会话生效）

1. **无请求不防御**：不写没人要求的 try/catch、回退链、参数校验、兼容层。
   异常让它崩进 zpin.log（`cpp/app/main.cpp` 装的 `set_terminate` 会接住）；只有调用方明确
   要求"失败时如何"才写对应处理。
2. **先测量再优化**：性能问题先埋点拿毫秒数，禁止凭感觉重写。
3. **不新增依赖**：任何第三方库先经用户确认。当前白名单：
   Qt 6.11（msvc2022_64，只用 Widgets/Svg/Gui/Core）、CMake ≥ 3.24、Ninja、
   MSVC 工具链、Rust stable + `cxx`/`cxx-build`、`windows` crate（Win32 绑定）、
   windeployqt（随 Qt 提供）、`oar-ocr` + `ort`（ONNX Runtime，文字识别）、
   `image`（RGB 缓冲构造）、OpenCV 4.10（core+imgproc **静态**库，智能擦除；
   一次性构建走 `tools\build_opencv.bat`，源码与产物都在**已忽略的** `build\opencv\`
   （`build\opencv\{opencv-4.10.0,build,install}`），不进版本库、不打进 zip 之外的分发包）。OCR 模型 PP-OCRv6 small 三个文件放 `assets/models/`，
   打包时拷到 exe 同级 `models\`，程序按自身路径找——不放 %LOCALAPPDATA%、不联网下载。
   来源与 SHA-256 见 `oar-ocr-core` 的 registry.rs（ModelScope `greatv/oar-ocr`）；
   换档位时注意**词典不通用**（tiny 用 27KB 精简版，small/medium 用 75KB 完整版）。
   旧的 PyQt6 / pyinstaller / maturin / pyo3 / pywin32-ctypes / pyflakes 随 Python
   实现一起作废。
4. **不碰 git 远端**：push/上传必须用户明示；本地也不主动 commit/stage。
5. **遵循现有风格**：中文注释、一个模块一件事、Win32 一律走 `cpp/sys/win32util.hpp`
   或 `rust/src/*`（不在界面代码里裸调 Win32）、常量与默认值进
   `cpp/sys/defaults.hpp`（版本号唯一来源）、UI 文案中文。
   C++ 模块按领域归档：`cpp/app/`（应用组装与共享服务）、`cpp/sys/`（系统交互
   与基础设施）、`cpp/capture/`（截图会话）、`cpp/pin/`（贴图）、`cpp/prefs/`
   （设置与帮助）；`#include` 一律平文件名，不写目录前缀。
6. **用户可见文案**（菜单/帮助/气泡）改动需先确认。
7. **改完必须自查**：`cargo test --release`（在 `rust/` 下）通过 + C++ 侧零警告零报错。
   ⚠️ `cmake --build build` **必须在 MSVC 环境里跑**：普通 PowerShell/cmd 直接执行会
   报 `qglobal.h(14): fatal error C1083: 无法打开包括文件: 'type_traits'`（缺 `INCLUDE`/`LIB`）。
   两条路都行：
   - `build\build.bat`（它第 62 行自己 `call vcvars64.bat`，顺带打包到 `dist\ZPin\`）；或
   - `cmd /c "call "<vcvars64.bat 路径>" && cmake --build build"`。
   临时单文件试编译的中间产物放 `build\scratch\`，**别让 `cl /c` 在源码目录留下 `.obj`**。
   界面行为改动要说明"需要手动验证什么"（本项目无自动化测试壳，用户已明确要求不加）。
   测不到的三类：真实按键触发热键、鼠标穿透、混合 DPI 贴图比例。构建产物全部进
   `build/`（CMake + cargo 同一个根），`cmake/` 是 CMake 脚本目录，别混淆。
8. **界面层是 C++/Qt6**（直接链 Qt，LGPLv3；不再经 PyQt6 绑定，GPL 传染义务随之消失）。
   只有纯计算与 Win32 深交互下沉到 Rust 核心（`rust/`，经 cxx 桥成静态库）——
   官方 Qt 的 Rust Bridge 只支持 QML，不支持 Widgets，别起「界面也用 Rust 写」的念头。
   枚举一律全限定（`Qt::WindowType::Tool` 这种写法）。
