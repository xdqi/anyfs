#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <napi.h>
#include <string>
#include <vector>

#ifdef _WIN32
// Delay-load hook: when ld.lld links us with --delayload=node.exe, the
// PE has a Delay Import directory for `node.exe`. There's no actual
// node.exe DLL on Electron systems (it's renamed to electron.exe /
// anyfs-demo.exe). This hook tells the delay-load helper to resolve
// napi_* symbols against the host EXE itself instead of LoadLibrary'ing
// a file called "node.exe".
// clang-format off
//   <windows.h> must precede <delayimp.h> — delayimp's prototypes use
//   HMODULE / FARPROC, which only become visible after windows.h. Lexical
//   header sort breaks the build.
#include <windows.h>
#include <delayimp.h>
// clang-format on

static FARPROC WINAPI anyfs_dli_hook(unsigned ev, PDelayLoadInfo info)
{
	if (ev == dliNotePreLoadLibrary && info->szDll &&
	    lstrcmpiA(info->szDll, "node.exe") == 0) {
		return reinterpret_cast<FARPROC>(GetModuleHandleW(NULL));
	}
	return nullptr;
}
extern "C" PfnDliHook __pfnDliNotifyHook2 = anyfs_dli_hook;
#endif

extern "C" {
int anyfs_ts_init(uint32_t mem_mb, uint32_t loglevel);
int anyfs_ts_kernel_halt(void);

const char* anyfs_get_last_error(void);

int anyfs_ts_disk_open(const char* image_path, uint32_t flags);
int anyfs_ts_disk_close(int h);
int anyfs_ts_disk_list_json(int h, char* buf, size_t cap);
int anyfs_ts_disk_meta_json(int h, char* buf, size_t cap);
int anyfs_ts_disk_enter(int h, unsigned int part, uint32_t flags,
			char* mount_out, size_t mount_cap);
int anyfs_ts_mount_whole(int h, const char* fstype, uint32_t flags,
			 char* mount_out, size_t mount_cap);

int anyfs_ts_readdir_json(const char* path, char* buf, size_t cap);
int anyfs_ts_lstat_json(const char* path, char* buf, size_t cap);
int anyfs_ts_stat_json(const char* path, char* buf, size_t cap);
int anyfs_ts_realpath(const char* path, char* buf, size_t cap);
int anyfs_ts_readlink(const char* path, char* buf, size_t cap);

int anyfs_ts_read_kernel_file(const char* path, char* buf, size_t cap);

int anyfs_ts_open(const char* path, int flags);
int64_t anyfs_ts_pread(int fd, void* buf, uint32_t n, int64_t off);
int anyfs_ts_close(int fd);
}

// Buffer-grow loop for the "negative rc = need this many bytes" protocol.
// Mirrors callJsonOut() in ts/packages/core/src/worker.ts:
//   rc >= 0          -> success, |rc| bytes valid in buf
//   rc < 0, -rc > cap-> overflow, retry with bigger buffer
//   rc < 0, -rc <= cap -> hard error, propagate
template <typename Fn>
static Napi::Value CallOverflowing(Napi::Env env, const char* name, Fn fn)
{
	size_t cap = 8192;
	for (int i = 0; i < 6; i++) {
		std::vector<char> buf(cap);
		int n = fn(buf.data(), buf.size());
		if (n >= 0)
			return Napi::String::New(env, buf.data(), (size_t)n);
		size_t need = (size_t)(-n);
		if (need <= cap) {
			Napi::Error::New(env, std::string(name) +
						  ": rc=" + std::to_string(n))
			    .ThrowAsJavaScriptException();
			return env.Null();
		}
		cap = std::max(need + 256, cap * 2);
	}
	Napi::Error::New(env,
			 std::string(name) + ": keeps requesting more buffer")
	    .ThrowAsJavaScriptException();
	return env.Null();
}

// ── lifecycle ────────────────────────────────────────────────────────────

static Napi::Value Init_(const Napi::CallbackInfo& info)
{
	uint32_t mem = info[0].As<Napi::Number>().Uint32Value();
	uint32_t lvl = info[1].As<Napi::Number>().Uint32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_init(mem, lvl));
}

static Napi::Value KernelHalt(const Napi::CallbackInfo& info)
{
	return Napi::Number::New(info.Env(), anyfs_ts_kernel_halt());
}

// ── disk handle ──────────────────────────────────────────────────────────

static Napi::Value DiskOpen(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	uint32_t fl = info[1].As<Napi::Number>().Uint32Value();
	int rc = anyfs_ts_disk_open(p.c_str(), fl);
	if (rc < 0) {
		const char* err = anyfs_get_last_error();
		if (err && *err)
			Napi::Error::New(info.Env(), err)
			    .ThrowAsJavaScriptException();
		else
			Napi::Error::New(info.Env(), "diskOpen failed")
			    .ThrowAsJavaScriptException();
		return info.Env().Undefined();
	}
	return Napi::Number::New(info.Env(), rc);
}

static Napi::Value DiskClose(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_disk_close(h));
}

static Napi::Value DiskListJson(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	return CallOverflowing(info.Env(), "diskListJson",
			       [h](char* b, size_t c) {
				       return anyfs_ts_disk_list_json(h, b, c);
			       });
}

static Napi::Value DiskMetaJson(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	return CallOverflowing(info.Env(), "diskMetaJson",
			       [h](char* b, size_t c) {
				       return anyfs_ts_disk_meta_json(h, b, c);
			       });
}

// disk_enter/mount_whole write the LKL mount path into a small buffer and
// return rc; the wrapper returns the mount path as a string and throws on rc<0.
static Napi::Value DiskEnter(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	uint32_t part = info[1].As<Napi::Number>().Uint32Value();
	uint32_t flags = info[2].As<Napi::Number>().Uint32Value();
	char out[256] = {0};
	int rc = anyfs_ts_disk_enter(h, part, flags, out, sizeof(out));
	if (rc != 0) {
		Napi::Error::New(info.Env(),
				 "diskEnter: rc=" + std::to_string(rc))
		    .ThrowAsJavaScriptException();
		return info.Env().Null();
	}
	return Napi::String::New(info.Env(), out);
}

static Napi::Value MountWhole(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	std::string fs =
	    info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "";
	uint32_t flags = info[2].As<Napi::Number>().Uint32Value();
	char out[256] = {0};
	int rc = anyfs_ts_mount_whole(h, fs.c_str(), flags, out, sizeof(out));
	if (rc != 0) {
		Napi::Error::New(info.Env(),
				 "mountWhole: rc=" + std::to_string(rc))
		    .ThrowAsJavaScriptException();
		return info.Env().Null();
	}
	return Napi::String::New(info.Env(), out);
}

// ── path ops ─────────────────────────────────────────────────────────────

static Napi::Value ReaddirJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	return CallOverflowing(
	    info.Env(), "readdirJson", [&p](char* b, size_t c) {
		    return anyfs_ts_readdir_json(p.c_str(), b, c);
	    });
}

static Napi::Value LstatJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	return CallOverflowing(
	    info.Env(), "lstatJson", [&p](char* b, size_t c) {
		    return anyfs_ts_lstat_json(p.c_str(), b, c);
	    });
}

static Napi::Value StatJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	return CallOverflowing(info.Env(), "statJson", [&p](char* b, size_t c) {
		return anyfs_ts_stat_json(p.c_str(), b, c);
	});
}

static Napi::Value Realpath_(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	return CallOverflowing(info.Env(), "realpath", [&p](char* b, size_t c) {
		return anyfs_ts_realpath(p.c_str(), b, c);
	});
}

// readlink doesn't use the overflow protocol — it's a thin wrapper around
// lkl_sys_readlink, which returns bytes-written or a negative errno.
static Napi::Value Readlink_(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	size_t cap = 4096;
	std::vector<char> buf(cap);
	int n = anyfs_ts_readlink(p.c_str(), buf.data(), buf.size());
	if (n < 0) {
		Napi::Error::New(info.Env(),
				 "readlink: rc=" + std::to_string(n))
		    .ThrowAsJavaScriptException();
		return info.Env().Null();
	}
	return Napi::String::New(info.Env(), buf.data(), (size_t)n);
}

// ── kernel file ────────────────────────────────────────────────────────────

static Napi::Value ReadKernelFile(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	return CallOverflowing(
	    info.Env(), "readKernelFile", [&p](char* b, size_t c) {
		    return anyfs_ts_read_kernel_file(p.c_str(), b, c);
	    });
}

// ── file ops ─────────────────────────────────────────────────────────────

static Napi::Value FileOpen(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	int flags = info[1].As<Napi::Number>().Int32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_open(p.c_str(), flags));
}

// pread(fd, dstBuffer, n, offset) — dstBuffer is a Node Buffer or Uint8Array.
// `n` may be smaller than the buffer length to limit the read.
// `offset` is a JS number; for >2^53 use diskMetaJson + chunked reads.
static Napi::Value Pread(const Napi::CallbackInfo& info)
{
	int fd = info[0].As<Napi::Number>().Int32Value();
	auto ta = info[1].As<Napi::Uint8Array>();
	uint32_t n = info[2].As<Napi::Number>().Uint32Value();
	int64_t off = (int64_t)info[3].As<Napi::Number>().Int64Value();
	if (n > ta.ByteLength())
		n = (uint32_t)ta.ByteLength();
	int64_t got = anyfs_ts_pread(fd, ta.Data(), n, off);
	return Napi::Number::New(info.Env(), (double)got);
}

static Napi::Value FileClose(const Napi::CallbackInfo& info)
{
	int fd = info[0].As<Napi::Number>().Int32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_close(fd));
}

// ── exports ──────────────────────────────────────────────────────────────

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
	exports.Set("init", Napi::Function::New(env, Init_));
	exports.Set("kernelHalt", Napi::Function::New(env, KernelHalt));

	exports.Set("diskOpen", Napi::Function::New(env, DiskOpen));
	exports.Set("diskClose", Napi::Function::New(env, DiskClose));
	exports.Set("diskListJson", Napi::Function::New(env, DiskListJson));
	exports.Set("diskMetaJson", Napi::Function::New(env, DiskMetaJson));
	exports.Set("diskEnter", Napi::Function::New(env, DiskEnter));
	exports.Set("mountWhole", Napi::Function::New(env, MountWhole));

	exports.Set("readKernelFile", Napi::Function::New(env, ReadKernelFile));
	exports.Set("readdirJson", Napi::Function::New(env, ReaddirJson));
	exports.Set("lstatJson", Napi::Function::New(env, LstatJson));
	exports.Set("statJson", Napi::Function::New(env, StatJson));
	exports.Set("realpath", Napi::Function::New(env, Realpath_));
	exports.Set("readlink", Napi::Function::New(env, Readlink_));

	exports.Set("fileOpen", Napi::Function::New(env, FileOpen));
	exports.Set("pread", Napi::Function::New(env, Pread));
	exports.Set("fileClose", Napi::Function::New(env, FileClose));
	return exports;
}

NODE_API_MODULE(anyfs_native, InitModule)
