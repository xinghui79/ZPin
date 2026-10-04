//! HTTP(S) 拉取 —— 自动更新的网络层：release 元数据与安装包都走 WinHTTP。
//!
//! 同步阻塞客户端，调用方（C++ 设置页/启动检查）必须放在工作线程。
//! 失败一律返回 Err：文案会经 cxx 桥的 rust::Error 直接进日志与设置页。
//! WinHTTP 默认校验证书、自动跟随重定向（GitHub 资产会跳 CDN），这里不做额外配置。

use std::fs::File;
use std::io::Write;

use windows::core::{Error as WinError, HSTRING, PCWSTR, w};
use windows::Win32::Networking::WinHttp::{
    WinHttpAddRequestHeaders, WinHttpCloseHandle, WinHttpConnect, WinHttpOpen,
    WinHttpOpenRequest, WinHttpQueryDataAvailable, WinHttpQueryHeaders, WinHttpReadData,
    WinHttpReceiveResponse, WinHttpSendRequest, WinHttpSetTimeouts,
    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_ADDREQ_FLAG_ADD, WINHTTP_FLAG_SECURE,
    WINHTTP_OPEN_REQUEST_FLAGS, WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_QUERY_STATUS_CODE,
};

type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// 最近一次 Win32 错误的可读文本（句柄创建类 API 不走 Result，得手动取）。
fn win_err() -> String {
    WinError::from_win32().to_string()
}

/// 拆 URL：返回 (是否 https, 主机, 端口, 路径含开头的 /)。只认 http/https。
/// 支持IPv6 字面量主机（[::1]:8080 形式，host 保留方括号）与
/// #fragment（fragment 不进请求行，直接剥掉）。
fn parse_url(url: &str) -> Result<(bool, String, u16, String), BoxError> {
    let bad = |msg: &str| -> BoxError { format!("URL 无法解析（{msg}）：{url}").into() };
    let Some((scheme, rest)) = url.split_once("://") else {
        return Err(bad("缺 scheme"));
    };
    let secure = match scheme {
        "https" => true,
        "http" => false,
        _ => return Err(bad("只支持 http/https")),
    };
    // fragment 不属于发往服务器的请求行，先剥掉
    let rest = rest.split('#').next().unwrap_or(rest);
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let (host, port) = if let Some(rest6) = authority.strip_prefix('[') {
        // IPv6 字面量：host 是 [..] 整段（WinHTTP 接受带方括号的主机），端口在 ]: 之后
        let Some((h, after)) = rest6.split_once(']') else {
            return Err(bad("IPv6 字面量缺右方括号"));
        };
        match after.strip_prefix(':') {
            Some(p) => (format!("[{h}]"), p.parse::<u16>().map_err(|_| bad("端口不是数字"))?),
            None => (format!("[{h}]"), if secure { 443 } else { 80 }),
        }
    } else {
        match authority.rsplit_once(':') {
            Some((h, p)) => (h.to_string(), p.parse::<u16>().map_err(|_| bad("端口不是数字"))?),
            None => (authority.to_string(), if secure { 443 } else { 80 }),
        }
    };
    if host.is_empty() {
        return Err(bad("主机为空"));
    }
    Ok((secure, host, port, path.to_string()))
}

/// WinHTTP 句柄 RAII：中途出错也把 session/connect/request 逐层关掉。
struct Handle(*mut std::ffi::c_void);

impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe {
                let _ = WinHttpCloseHandle(self.0);
            }
        }
    }
}

/// 一次 GET 的完整句柄组。字段顺序即 Drop 顺序：先关 request 再关父句柄，
/// 反过来关父句柄会连带作废子句柄（WinHTTP 文档明确不让这么干）。
struct HttpReply {
    request: Handle,
    _connect: Handle,
    _session: Handle,
}

impl HttpReply {
    fn status(&self) -> Result<u32, BoxError> {
        let mut code: u32 = 0;
        let mut size = std::mem::size_of::<u32>() as u32;
        unsafe {
            WinHttpQueryHeaders(
                self.request.0,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                PCWSTR::null(),
                Some((&mut code as *mut u32).cast()),
                &mut size,
                &mut 0,
            )
        }?;
        Ok(code)
    }

    /// 把响应体逐块交给 push，返回累计字节数。
    fn read_to(&self, mut push: impl FnMut(&[u8]) -> std::io::Result<()>) -> Result<u64, BoxError> {
        let mut total: u64 = 0;
        loop {
            let mut avail: u32 = 0;
            unsafe { WinHttpQueryDataAvailable(self.request.0, &mut avail) }?;
            if avail == 0 {
                return Ok(total);
            }
            let mut buf = vec![0u8; avail as usize];
            let mut got: u32 = 0;
            unsafe { WinHttpReadData(self.request.0, buf.as_mut_ptr().cast(), avail, &mut got) }?;
            if got == 0 {
                return Ok(total);
            }
            push(&buf[..got as usize]).map_err(|e| format!("处理下载内容失败：{e}"))?;
            total += u64::from(got);
        }
    }
}

/// GET 一个 URL，校验 2xx 后返回可读体的响应。headers 是附加请求头（可空）。
fn request(url: &str, headers: &str) -> Result<HttpReply, BoxError> {
    let (secure, host, port, path) = parse_url(url)?;
    unsafe {
        let session = Handle(WinHttpOpen(
            w!("ZPin"),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            PCWSTR::null(),
            PCWSTR::null(),
            0,
        ));
        if session.0.is_null() {
            return Err(format!("初始化 WinHTTP 失败：{}", win_err()).into());
        }
        // 域名解析/连接/发送别拖过 15s，收体给足 60s（几十 MB 的安装包）
        WinHttpSetTimeouts(session.0, 10_000, 15_000, 15_000, 60_000)?;
        let host_str = HSTRING::from(&host);
        let connect = Handle(WinHttpConnect(session.0, &host_str, port, 0));
        if connect.0.is_null() {
            return Err(format!("连接 {host} 失败：{}", win_err()).into());
        }
        let path_str = HSTRING::from(path);
        let request = Handle(WinHttpOpenRequest(
            connect.0,
            w!("GET"),
            &path_str,
            PCWSTR::null(),
            PCWSTR::null(),
            std::ptr::null(),
            if secure {
                WINHTTP_FLAG_SECURE
            } else {
                WINHTTP_OPEN_REQUEST_FLAGS(0)
            },
        ));
        if request.0.is_null() {
            return Err(format!("创建请求失败：{}", win_err()).into());
        }
        if !headers.is_empty() {
            let utf16: Vec<u16> = headers.encode_utf16().collect();
            WinHttpAddRequestHeaders(request.0, &utf16, WINHTTP_ADDREQ_FLAG_ADD)?;
        }
        WinHttpSendRequest(request.0, None, None, 0, 0, 0)?;
        WinHttpReceiveResponse(request.0, std::ptr::null_mut())?;
        let reply = HttpReply {
            request,
            _connect: connect,
            _session: session,
        };
        let status = reply.status()?;
        if !(200..300).contains(&status) {
            return Err(format!("HTTP {status}").into());
        }
        Ok(reply)
    }
}

/// HTTP(S) GET 整个响应体；非 2xx 或网络失败返回 Err。
pub fn http_get(url: &str, headers: &str) -> Result<Vec<u8>, BoxError> {
    let reply = request(url, headers)?;
    let mut body = Vec::new();
    reply.read_to(|chunk| {
        body.extend_from_slice(chunk);
        Ok(())
    })?;
    Ok(body)
}

/// 流式下载到本地文件（覆盖写），返回字节数；失败返回 Err 并清掉半截文件
/// （调用方按字节数+哈希校验，残包留着只会误导下一次重试前的判断）。
pub fn download_to_file(url: &str, dest: &str, headers: &str) -> Result<u64, BoxError> {
    let reply = match request(url, headers) {
        Ok(r) => r,
        Err(e) => {
            let _ = std::fs::remove_file(dest);
            return Err(e);
        }
    };
    let mut file = match File::create(dest) {
        Ok(f) => f,
        Err(e) => return Err(format!("无法写入 {dest}：{e}").into()),
    };
    let total = match reply.read_to(|chunk| file.write_all(chunk)) {
        Ok(n) => n,
        Err(e) => {
            drop(file);
            let _ = std::fs::remove_file(dest);
            return Err(e);
        }
    };
    file.flush().map_err(|e| format!("写盘收尾失败：{e}"))?;
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::parse_url;

    #[test]
    fn https_host_and_path() {
        let (secure, host, port, path) =
            parse_url("https://api.github.com/repos/xinghui79/ZPin/releases/latest").unwrap();
        assert!(secure);
        assert_eq!(host, "api.github.com");
        assert_eq!(port, 443);
        assert_eq!(path, "/repos/xinghui79/ZPin/releases/latest");
    }

    #[test]
    fn defaults_and_explicit_port() {
        let (secure, host, port, path) = parse_url("http://example.com").unwrap();
        assert!(!secure);
        assert_eq!(host, "example.com");
        assert_eq!(port, 80);
        assert_eq!(path, "/");
        let (_, _, port, _) = parse_url("https://example.com:8443/a/b").unwrap();
        assert_eq!(port, 8443);
    }

    #[test]
    fn ipv6_host_port_and_fragment() {
        let (secure, host, port, path) = parse_url("http://[::1]:8080/x#frag").unwrap();
        assert!(!secure);
        assert_eq!(host, "[::1]");
        assert_eq!(port, 8080);
        assert_eq!(path, "/x");
        let (_, host2, port2, path2) = parse_url("https://[2001:db8::1]/y").unwrap();
        assert_eq!(host2, "[2001:db8::1]");
        assert_eq!(port2, 443);
        assert_eq!(path2, "/y");
    }

    #[test]
    fn rejects_bad_urls() {
        assert!(parse_url("ftp://example.com/x").is_err());
        assert!(parse_url("example.com/x").is_err());
        assert!(parse_url("https://:443/x").is_err());
    }
}
