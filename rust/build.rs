//! 构建脚本：把 cxx 桥接生成的 C++ 编译进静态库，
//! 并把头文件镜像到稳定的 target/<profile>/cxxbridge/ 供 CMake 直接包含。

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    cxx_build::bridge("src/lib.rs")
        .flag_if_supported("/std:c++20")
        // 源码注释是 UTF-8 中文；cl 默认按系统代码页(936)读会语法错乱
        .flag_if_supported("/utf-8")
        .compile("zpin_core_bridge");
    // 对整个 src 目录声明，而不是逐个文件列：早先这里手列了 7 个模块，
    // 实际有 9 个——content.rs 与 ocr.rs 漏了。cargo 自己按模块追踪所以功能
    // 侥幸没坏，但这份清单每加一个模块就会再漂一次，且一旦有人在这些文件里
    // 加 include! 或 #[cxx::bridge] 片段，桥接头不会重新生成。
    println!("cargo:rerun-if-changed=src");
    copy_headers();
}

/// cxx 生成的头文件落在带 hash 的 OUT_DIR 深处；拷到 target/<profile>/cxxbridge/
/// （剥掉可能存在的顶层 cxxbridge 目录名），CMake 侧即可稳定地
/// #include "zpin-core/src/lib.rs.h"（crate 名做目录名，见 rcore.cpp）。
fn copy_headers() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let profile_dir = out_dir
        .ancestors()
        .nth(3)
        .expect("OUT_DIR 应位于 target/<profile>/build/<crate>-<hash>/out");
    let dst_root = profile_dir.join("cxxbridge");
    copy_tree(&out_dir, &dst_root, &out_dir).unwrap();
}

fn copy_tree(src: &Path, dst_root: &Path, top: &Path) -> std::io::Result<()> {
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let ty = entry.file_type()?;
        let from = entry.path();
        if ty.is_dir() {
            copy_tree(&from, dst_root, top)?;
        } else if from.extension().map(|e| e == "h").unwrap_or(false) {
            let rel = from.strip_prefix(top).unwrap();
            let rel = rel.strip_prefix("cxxbridge").unwrap_or(rel);
            let to = dst_root.join(rel);
            fs::create_dir_all(to.parent().unwrap())?;
            fs::copy(&from, &to)?;
        }
    }
    Ok(())
}
