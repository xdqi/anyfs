#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
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
int anyfs_ts_kernel_init(uint32_t mem_mb, uint32_t loglevel);
int anyfs_ts_kernel_halt(void);

const char* anyfs_get_last_error(void);

int anyfs_ts_session_open(const char* image_path, uint32_t flags);
int anyfs_ts_session_close(int h);
int anyfs_ts_session_list_json(int h, char* buf, size_t cap);
int anyfs_ts_session_meta_json(int h, char* buf, size_t cap);
int anyfs_ts_session_enter(int h, unsigned int part, uint32_t flags,
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

// ── serialization ──────────────────────────────────────────────────────────
// LKL holds a single CPU lock; two concurrent ops would crash the kernel.
// AsyncWorker's Execute() runs in the libuv thread pool; this mutex
// guarantees only one op touches LKL at a time. Later JS calls queue on
// the mutex — they do NOT block the JS main thread.
static std::mutex g_op_mutex;

// ── AsyncWorker helpers ──────────────────────────────────────────────────

// Simple int-return C call. OnOK resolves with the integer rc.
class IntRetWorker : public Napi::AsyncWorker
{
      public:
	IntRetWorker(Napi::Env env, Napi::Promise::Deferred deferred,
		     std::function<int()> fn)
	    : Napi::AsyncWorker(env), deferred_(std::move(deferred)),
	      fn_(std::move(fn)), rc_(-1)
	{
	}

	void Execute() override
	{
		std::lock_guard<std::mutex> lock(g_op_mutex);
		rc_ = fn_();
	}

	void OnOK() override
	{
		deferred_.Resolve(Napi::Number::New(Env(), rc_));
	}

	void OnError(const Napi::Error& e) override
	{
		deferred_.Reject(e.Value());
	}

      private:
	Napi::Promise::Deferred deferred_;
	std::function<int()> fn_;
	int rc_;
};

// Buffer-grow JSON-out worker. Mirrors the old CallOverflowing helper but
// runs in the thread pool. Result string is resolved when rc >= 0.
class JsonOutWorker : public Napi::AsyncWorker
{
      public:
	JsonOutWorker(Napi::Env env, Napi::Promise::Deferred deferred,
		      std::string name, std::function<int(char*, size_t)> fn)
	    : Napi::AsyncWorker(env), deferred_(std::move(deferred)),
	      name_(std::move(name)), fn_(std::move(fn)), err_("")
	{
	}

	void Execute() override
	{
		std::lock_guard<std::mutex> lock(g_op_mutex);
		size_t cap = 8192;
		for (int i = 0; i < 6; i++) {
			std::vector<char> buf(cap);
			int n = fn_(buf.data(), buf.size());
			if (n >= 0) {
				result_ = std::string(buf.data(), (size_t)n);
				return;
			}
			size_t need = (size_t)(-n);
			if (need <= cap) {
				err_ = name_ + ": rc=" + std::to_string(n);
				return;
			}
			cap = std::max(need + 256, cap * 2);
		}
		err_ = name_ + ": keeps requesting more buffer";
	}

	void OnOK() override
	{
		if (!err_.empty()) {
			Napi::Error::New(Env(), err_)
			    .ThrowAsJavaScriptException();
			deferred_.Resolve(Env().Null());
			return;
		}
		deferred_.Resolve(
		    Napi::String::New(Env(), result_.data(), result_.size()));
	}

	void OnError(const Napi::Error& e) override
	{
		deferred_.Reject(e.Value());
	}

      private:
	Napi::Promise::Deferred deferred_;
	std::string name_;
	std::function<int(char*, size_t)> fn_;
	std::string result_;
	std::string err_;
};

// String-out worker with a pre-sized stack buffer. The C function writes
// into the buffer and returns rc; on rc==0 OnOK resolves with the string.
class StrOutWorker : public Napi::AsyncWorker
{
      public:
	StrOutWorker(Napi::Env env, Napi::Promise::Deferred deferred,
		     size_t buf_cap, std::function<int(char*, size_t, int*)> fn)
	    : Napi::AsyncWorker(env), deferred_(std::move(deferred)),
	      buf_(buf_cap), fn_(std::move(fn)), rc_(-1)
	{
	}

	void Execute() override
	{
		std::lock_guard<std::mutex> lock(g_op_mutex);
		int n = 0;
		rc_ = fn_(buf_.data(), buf_.size(), &n);
		if (rc_ == 0)
			result_ = std::string(buf_.data(), (size_t)n);
	}

	void OnOK() override
	{
		if (rc_ != 0) {
			Napi::Error::New(Env(),
					 "operation: rc=" + std::to_string(rc_))
			    .ThrowAsJavaScriptException();
			deferred_.Resolve(Env().Null());
			return;
		}
		deferred_.Resolve(
		    Napi::String::New(Env(), result_.data(), result_.size()));
	}

	void OnError(const Napi::Error& e) override
	{
		deferred_.Reject(e.Value());
	}

      private:
	Napi::Promise::Deferred deferred_;
	std::vector<char> buf_;
	std::function<int(char*, size_t, int*)> fn_;
	std::string result_;
	int rc_;
};

// pread worker — mallocs a temp buffer, calls anyfs_ts_pread, resolves
// with { rc: number, data: Uint8Array }.
class PreadWorker : public Napi::AsyncWorker
{
      public:
	PreadWorker(Napi::Env env, Napi::Promise::Deferred deferred, int fd,
		    uint32_t n, int64_t off)
	    : Napi::AsyncWorker(env), deferred_(std::move(deferred)), fd_(fd),
	      n_(n), off_(off), rc_(-1)
	{
	}

	void Execute() override
	{
		std::lock_guard<std::mutex> lock(g_op_mutex);
		if (n_ > 0) {
			buf_.resize(n_);
			rc_ = (int)anyfs_ts_pread(fd_, buf_.data(), n_, off_);
		} else {
			rc_ = 0;
		}
	}

	void OnOK() override
	{
		auto obj = Napi::Object::New(Env());
		obj.Set("rc", Napi::Number::New(Env(), rc_));
		if (rc_ > 0) {
			obj.Set("data", Napi::Buffer<uint8_t>::Copy(
					    Env(), buf_.data(), (size_t)rc_));
		} else {
			obj.Set("data", Napi::Buffer<uint8_t>::New(Env(), 0));
		}
		deferred_.Resolve(obj);
	}

	void OnError(const Napi::Error& e) override
	{
		deferred_.Reject(e.Value());
	}

      private:
	Napi::Promise::Deferred deferred_;
	int fd_;
	uint32_t n_;
	int64_t off_;
	std::vector<uint8_t> buf_;
	int rc_;
};

// ── lifecycle ────────────────────────────────────────────────────────────
// kernelInit / kernelHalt / sessionOpen / sessionClose stay sync because
// QEMU's blk_new_open / blk_unref assert qemu_in_main_thread().  Moving
// these to a thread pool would require a dedicated LKL thread; for now the
// IO-heavy ops (listParts, enter, readdir, pread, …) are the ones that
// actually block the event loop for significant time.

static Napi::Value KernelInit(const Napi::CallbackInfo& info)
{
	std::lock_guard<std::mutex> lock(g_op_mutex);
	uint32_t mem = info[0].As<Napi::Number>().Uint32Value();
	uint32_t lvl = info[1].As<Napi::Number>().Uint32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_kernel_init(mem, lvl));
}

static Napi::Value KernelHalt(const Napi::CallbackInfo& info)
{
	return Napi::Number::New(info.Env(), anyfs_ts_kernel_halt());
}

// ── disk handle ──────────────────────────────────────────────────────────

static Napi::Value SessionOpen(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	uint32_t fl = info[1].As<Napi::Number>().Uint32Value();
	std::lock_guard<std::mutex> lock(g_op_mutex);
	int rc = anyfs_ts_session_open(p.c_str(), fl);
	if (rc < 0) {
		const char* err = anyfs_get_last_error();
		if (err && *err)
			Napi::Error::New(info.Env(), err)
			    .ThrowAsJavaScriptException();
		else
			Napi::Error::New(info.Env(), "sessionOpen failed")
			    .ThrowAsJavaScriptException();
		return info.Env().Undefined();
	}
	return Napi::Number::New(info.Env(), rc);
}

static Napi::Value SessionClose(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	return Napi::Number::New(info.Env(), anyfs_ts_session_close(h));
}

// ── IO ops (async via thread pool) ──────────────────────────────────────

static Napi::Value SessionListJson(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred),
				    "sessionListJson", [h](char* b, size_t c) {
					    return anyfs_ts_session_list_json(
						h, b, c);
				    });
	w->Queue();
	return promise;
}

static Napi::Value SessionMetaJson(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred),
				    "sessionMetaJson", [h](char* b, size_t c) {
					    return anyfs_ts_session_meta_json(
						h, b, c);
				    });
	w->Queue();
	return promise;
}

// SessionEnter (partition mount) is SYNCHRONOUS, unlike the other IO ops.
// Running the ext4/btrfs mount on a libuv thread-pool thread (the AsyncWorker
// path) deadlocks/crashes inside the Electron main process: QEMU's libblock
// AioContext gets dispatched by Electron's GLib main loop and collides with
// QEMU's own io_uring/epoll fdmon driving the same ring (see FINDINGS F7 — the
// SIGABRT at fdmon-io_uring.c get_sqe). The pre-3388c5b synchronous behavior
// runs the mount on the calling (main) thread, which avoids the foreign-loop
// entanglement and mounts cleanly. The cost is a brief main-thread block for
// the duration of the mount — acceptable vs. a hang/crash. (Node/CLI keep
// working; only the threading changed.)
static Napi::Value SessionEnter(const Napi::CallbackInfo& info)
{
	int h = info[0].As<Napi::Number>().Int32Value();
	uint32_t part = info[1].As<Napi::Number>().Uint32Value();
	uint32_t flags = info[2].As<Napi::Number>().Uint32Value();
	char out[256];
	int rc;
	{
		std::lock_guard<std::mutex> lock(g_op_mutex);
		rc = anyfs_ts_session_enter(h, part, flags, out, sizeof(out));
	}
	if (rc < 0) {
		const char* err = anyfs_get_last_error();
		Napi::Error::New(info.Env(),
				 (err && *err) ? err : "sessionEnter failed")
		    .ThrowAsJavaScriptException();
		return info.Env().Undefined();
	}
	return Napi::String::New(info.Env(), out);
}

// ── path ops ─────────────────────────────────────────────────────────────

static Napi::Value ReaddirJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred),
				    "readdirJson", [p](char* b, size_t c) {
					    return anyfs_ts_readdir_json(
						p.c_str(), b, c);
				    });
	w->Queue();
	return promise;
}

static Napi::Value LstatJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred),
				    "lstatJson", [p](char* b, size_t c) {
					    return anyfs_ts_lstat_json(
						p.c_str(), b, c);
				    });
	w->Queue();
	return promise;
}

static Napi::Value StatJson(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred), "statJson",
				    [p](char* b, size_t c) {
					    return anyfs_ts_stat_json(p.c_str(),
								      b, c);
				    });
	w->Queue();
	return promise;
}

static Napi::Value Realpath_(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w =
	    new JsonOutWorker(info.Env(), std::move(deferred), "realpath",
			      [p](char* b, size_t c) {
				      return anyfs_ts_realpath(p.c_str(), b, c);
			      });
	w->Queue();
	return promise;
}

static Napi::Value Readlink_(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new StrOutWorker(info.Env(), std::move(deferred), 4096,
				   [p](char* out, size_t cap, int* n) {
					   *n = anyfs_ts_readlink(p.c_str(),
								  out, cap);
					   return *n < 0 ? *n : 0;
				   });
	w->Queue();
	return promise;
}

// ── kernel file ──────────────────────────────────────────────────────────

static Napi::Value ReadKernelFile(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new JsonOutWorker(info.Env(), std::move(deferred),
				    "readKernelFile", [p](char* b, size_t c) {
					    return anyfs_ts_read_kernel_file(
						p.c_str(), b, c);
				    });
	w->Queue();
	return promise;
}

// ── file ops ─────────────────────────────────────────────────────────────

static Napi::Value FileOpen(const Napi::CallbackInfo& info)
{
	std::string p = info[0].As<Napi::String>();
	int flags = info[1].As<Napi::Number>().Int32Value();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new IntRetWorker(info.Env(), std::move(deferred), [p, flags] {
		return anyfs_ts_open(p.c_str(), flags);
	});
	w->Queue();
	return promise;
}

// pread(fd, n, off) — the buffer is now allocated + returned by the
// AsyncWorker.  Signature changes from (fd, buf, n, off) to (fd, n, off);
// returns Promise<{ rc: number, data: Uint8Array }>.
static Napi::Value Pread(const Napi::CallbackInfo& info)
{
	int fd = info[0].As<Napi::Number>().Int32Value();
	uint32_t n = info[1].As<Napi::Number>().Uint32Value();
	int64_t off = (int64_t)info[2].As<Napi::Number>().Int64Value();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new PreadWorker(info.Env(), std::move(deferred), fd, n, off);
	w->Queue();
	return promise;
}

static Napi::Value FileClose(const Napi::CallbackInfo& info)
{
	int fd = info[0].As<Napi::Number>().Int32Value();
	auto deferred = Napi::Promise::Deferred::New(info.Env());
	auto promise = deferred.Promise();
	auto* w = new IntRetWorker(info.Env(), std::move(deferred),
				   [fd] { return anyfs_ts_close(fd); });
	w->Queue();
	return promise;
}

// ── exports ──────────────────────────────────────────────────────────────

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
	exports.Set("kernelInit", Napi::Function::New(env, KernelInit));
	exports.Set("kernelHalt", Napi::Function::New(env, KernelHalt));

	exports.Set("sessionOpen", Napi::Function::New(env, SessionOpen));
	exports.Set("sessionClose", Napi::Function::New(env, SessionClose));
	exports.Set("sessionListJson",
		    Napi::Function::New(env, SessionListJson));
	exports.Set("sessionMetaJson",
		    Napi::Function::New(env, SessionMetaJson));
	exports.Set("sessionEnter", Napi::Function::New(env, SessionEnter));

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
