/* WebAssembly's native execution/JS boundary. WABT is Apache-2.0 licensed.
 * The interpreter owns linear storage; QuickJS ArrayBuffers alias that storage.
 * wasm2c/TCC executes typed native code over those same resources.
 */
#include "js_wasm.h"
#include "wabt/binary-reader.h"
#include "wabt/interp/binary-reader-interp.h"
#include "wabt/interp/interp.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/c-writer.h"
#include "wabt/generate-names.h"
#include "wabt/apply-names.h"
#include "wabt/stream.h"
#include "wasm-rt-host.h"
#include "libtcc.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace wi = wabt::interp;
using wabt::Type;
using wabt::Result;
enum Kind { StateKind, ModuleKind, InstanceKind, MemoryKind, TableKind,
            GlobalKind, FunctionKind, ForeignKind, ConstructorKind,
            SuspendingKind, PromiseTaskKind, KindCount };
enum ErrorKind { CompileError, LinkError, RuntimeError, SuspendError, ErrorCount };
static JSClassID classes[KindCount];
struct State;
struct Handle;
struct Active;
struct PromiseTask;
struct SuspendingData { JSValue function; };
static void task_finalize(PromiseTask *);
static void task_mark(JSRuntime *, PromiseTask *, JS_MarkFunc *);
static void refresh_execution_roots(State *);
static std::map<JSContext *, std::weak_ptr<State>> states;
struct ConstructorData {
  std::shared_ptr<State> state;
  JSValue owner;
  JSCFunctionData *function;
  int magic;
  bool require_new;
};
struct Binding {
  State *state;
  JSValue value;
  Binding(State *, JSValueConst);
  ~Binding();
};
struct State : std::enable_shared_from_this<State> {
  JSRuntime *rt;
  JSContext *identity;
  web_js_wasm_stats stats = {};
  JSContext *active = nullptr;
  int (*poll)(JSContext *);
  int (*can_compile)(JSContext *);
  JSValue owner = JS_UNDEFINED; // weak; constructors and handles own this value
  JSValue errors[ErrorCount] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
  JSAtom buffer_owner = JS_ATOM_NULL;
  JSAtom iterator = JS_ATOM_NULL;
  JSValue data_view_getters[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
  std::vector<JSValue> *active_roots = nullptr;
  Active *active_frame = nullptr;
  PromiseTask *task = nullptr;
  std::vector<PromiseTask *> tasks;
  bool retired = false;
  std::map<size_t, JSValue> wrappers; // weak, erased by the wrapper finalizer
  std::map<void *, size_t> functions; // weak reverse map, no per-call store scan
  std::vector<Binding *> bindings;
  bool collecting = false;
  bool collection_pending = false;
  wasm_host_roots *native_roots = nullptr;
  std::map<size_t, JSValue> native_anchors;
  std::string native_fallback;
  // Destroy the native store before containers used by native finalizers.
  wi::Store store;
  bool cancellation_requested() const;
  bool interrupted();
  State(JSContext *ctx, int (*p)(JSContext *), int (*allow)(JSContext *))
      : rt(JS_GetRuntime(ctx)), identity(ctx), poll(p), can_compile(allow) {
    store.set_interrupt_callback([this]() {
      return interrupted();
    });
    store.set_execution_owner_callback([this]() -> void* { return task; });
  }
  ~State() {
    states.erase(identity);
    // Store destruction frees HostFunc/Foreign bindings, so their list must
    // remain alive until after the store has been collected.
    collecting = true;
    store.Collect();
    release_js_edges();
    JS_FreeAtomRT(rt, buffer_owner);
    JS_FreeAtomRT(rt, iterator);
  }
  void release_js_edges() {
    // The JS owner can be finalized before the last native Handle in a white
    // cycle. Release its marked edges at that JS finalizer, not at a later
    // shared_ptr destructor after the JS targets have already been reclaimed.
    for (JSValue &error : errors) {
      JSValue old = error; error = JS_UNDEFINED; JS_FreeValueRT(rt, old);
    }
    for (JSValue &getter : data_view_getters) {
      JSValue old = getter; getter = JS_UNDEFINED; JS_FreeValueRT(rt, old);
    }
    for (const auto& item : native_anchors) JS_FreeValueRT(rt, item.second);
    native_anchors.clear();
  }
  void collect() {
    collection_pending = true;
    if (!active) flush_collection();
  }
  void flush_collection() {
    if (!collecting && collection_pending) {
      collection_pending = false;
      collecting = true;
      store.Collect();
      collecting = false;
    }
  }
};
Binding::Binding(State *s, JSValueConst v) : state(s), value(JS_DupValueRT(s->rt, v)) {
  s->bindings.push_back(this);
}
Binding::~Binding() {
  auto &list = state->bindings;
  for (auto i = list.begin(); i != list.end(); ++i) {
    if (*i == this) { list.erase(i); break; }
  }
  JS_FreeValueRT(state->rt, value);
}
struct Handle {
  std::shared_ptr<State> lifetime;
  State *state;
  wi::Object::Ptr object;
  JSValue owner;
  JSValue buffer = JS_UNDEFINED;
  JSValue exports = JS_UNDEFINED;
  Binding *binding = nullptr; // native object owns it, this anchor marks its JS edge
  std::vector<JSValue> references;
  std::vector<uint8_t> binary;
  bool registered = false;
  void *function_identity = nullptr;
  Handle(State *s, wi::Ref ref) : lifetime(s->shared_from_this()), state(s), object(s->store, ref),
      owner(JS_DupValueRT(s->rt, s->owner)) {}
  ~Handle() {
    if (registered) state->wrappers.erase(object.ref().index);
    if (function_identity) state->functions.erase(function_identity);
    JS_FreeValueRT(state->rt, buffer);
    JS_FreeValueRT(state->rt, exports);
    for (JSValue ref : references) JS_FreeValueRT(state->rt, ref);
    // The anchor's physical JS edge must be released during its finalizer,
    // not after QuickJS has reclaimed a white cycle. Native reclamation alone
    // can safely wait for a task boundary.
    if (binding) {
      JSValue value = binding->value; binding->value = JS_UNDEFINED;
      JS_FreeValueRT(state->rt, value);
    }
    object.reset();
    state->collection_pending = true;
    JS_FreeValueRT(state->rt, owner);
  }
};
struct Active {
  State *state;
  JSContext *old;
  std::vector<JSValue> roots;
  std::vector<JSValue> *old_roots;
  Active *previous;
  Active(State *s, JSContext *ctx) : state(s), old(s->active), old_roots(s->active_roots), previous(s->active_frame) {
    s->active = ctx; s->active_roots = &roots;
    s->active_frame = this;
  }
  ~Active() {
    for (JSValue root : roots) JS_FreeValueRT(state->rt, root);
    state->active_roots = old_roots; state->active = old;
    state->active_frame = previous;
    if (!old) { refresh_execution_roots(state); state->flush_collection(); }
  }
};
static State *get_state(JSValueConst owner) {
  auto ptr = static_cast<std::shared_ptr<State> *>(JS_GetOpaque(owner, classes[StateKind]));
  return ptr ? ptr->get() : nullptr;
}
static Handle *handle(JSContext *ctx, JSValueConst value, Kind kind) {
  return static_cast<Handle *>(JS_GetOpaque2(ctx, value, classes[kind]));
}
static void finalizer(JSRuntime *rt, JSValue value) {
  JSClassID id;
  void *opaque = JS_GetAnyOpaque(value, &id);
  if (id == classes[ConstructorKind]) {
    auto c = static_cast<ConstructorData *>(opaque);
    if (c) { JS_FreeValueRT(c->state->rt, c->owner); delete c; }
  } else if (id == classes[SuspendingKind]) {
    auto data = static_cast<SuspendingData *>(opaque);
    if (data) { JS_FreeValueRT(rt, data->function); delete data; }
  } else if (id == classes[PromiseTaskKind]) {
    task_finalize(static_cast<PromiseTask *>(opaque));
  } else if (id == classes[StateKind]) {
    auto owner = static_cast<std::shared_ptr<State> *>(opaque);
    if (owner) { (*owner)->release_js_edges(); delete owner; }
  }
  else delete static_cast<Handle *>(opaque);
}
static void mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *fn) {
  JSClassID id;
  void *opaque = JS_GetAnyOpaque(value, &id);
  if (!opaque) return;
  if (id == classes[ConstructorKind]) {
    JS_MarkValue(rt, static_cast<ConstructorData *>(opaque)->owner, fn);
  } else if (id == classes[SuspendingKind]) {
    JS_MarkValue(rt, static_cast<SuspendingData *>(opaque)->function, fn);
  } else if (id == classes[PromiseTaskKind]) {
    task_mark(rt, static_cast<PromiseTask *>(opaque), fn);
  } else if (id == classes[StateKind]) {
    auto s = static_cast<std::shared_ptr<State> *>(opaque)->get();
    for (JSValue error : s->errors) JS_MarkValue(rt, error, fn);
    for (JSValue getter : s->data_view_getters) JS_MarkValue(rt, getter, fn);
  } else {
    auto h = static_cast<Handle *>(opaque);
    JS_MarkValue(rt, h->owner, fn);
    JS_MarkValue(rt, h->buffer, fn);
    JS_MarkValue(rt, h->exports, fn);
    if (h->binding) JS_MarkValue(rt, h->binding->value, fn);
    for (JSValue ref : h->references) JS_MarkValue(rt, ref, fn);
  }
}
static JSValue wasm_error(JSContext *ctx, State *s, ErrorKind kind,
                          const char *message) {
  JSValue error = JS_NewError(ctx);
  if (JS_IsException(error)) return error;
  JS_SetPrototype(ctx, error, s->errors[kind]);
  JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message));
  return JS_Throw(ctx, error);
}
static JSValue error_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                          JSValueConst *argv, int magic, JSValue *data) {
  State *s = get_state(data[0]);
  JSValue value = JS_NewError(ctx);
  if (JS_IsException(value)) return value;
  JSValue proto = JS_IsUndefined(new_target) ? JS_DupValue(ctx, s->errors[magic]) :
      JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsObject(proto)) JS_SetPrototype(ctx, value, proto);
  JS_FreeValue(ctx, proto);
  if (argc && !JS_IsUndefined(argv[0])) {
    JSValue message = JS_ToString(ctx, argv[0]);
    if (JS_IsException(message)) { JS_FreeValue(ctx, value); return message; }
    JS_DefinePropertyValueStr(ctx, value, "message", message,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  }
  return value;
}
static JSValue arg(int argc, JSValueConst *argv, int n) {
  return n < argc ? argv[n] : JS_UNDEFINED;
}
struct ValueList {
  JSContext *ctx;
  std::vector<JSValue> values;
  explicit ValueList(JSContext *c) : ctx(c) {}
  ~ValueList() { for (JSValue value : values) JS_FreeValue(ctx, value); }
};
static bool iterable_values(JSContext *ctx, State *s, JSValueConst value, ValueList *out) {
  if (s->cancellation_requested()) return false;
  JSValue method = JS_GetProperty(ctx, value, s->iterator);
  if (s->cancellation_requested()) { JS_FreeValue(ctx, method); return false; }
  if (JS_IsException(method)) return false;
  if (!JS_IsFunction(ctx, method)) {
    JS_FreeValue(ctx, method); JS_ThrowTypeError(ctx, "Expected an iterable WebAssembly value list"); return false;
  }
  JSValue iterator = JS_Call(ctx, method, value, 0, nullptr); JS_FreeValue(ctx, method);
  if (s->cancellation_requested()) { JS_FreeValue(ctx, iterator); return false; }
  if (JS_IsException(iterator)) return false;
  if (!JS_IsObject(iterator)) {
    JS_FreeValue(ctx, iterator); JS_ThrowTypeError(ctx, "Iterator must return an object"); return false;
  }
  JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
  bool ok = !JS_IsException(next) && !s->cancellation_requested();
  while (ok) {
    JSValue step = JS_Call(ctx, next, iterator, 0, nullptr);
    if (s->cancellation_requested()) { JS_FreeValue(ctx, step); ok = false; break; }
    if (JS_IsException(step)) { ok = false; break; }
    if (!JS_IsObject(step)) {
      JS_FreeValue(ctx, step); JS_ThrowTypeError(ctx, "Iterator result must be an object"); ok = false; break;
    }
    JSValue done = JS_GetPropertyStr(ctx, step, "done");
    int finished = JS_IsException(done) ? -1 : JS_ToBool(ctx, done); JS_FreeValue(ctx, done);
    if (s->cancellation_requested()) { JS_FreeValue(ctx, step); ok = false; break; }
    if (finished) { JS_FreeValue(ctx, step); ok = finished > 0; break; }
    JSValue item = JS_GetPropertyStr(ctx, step, "value"); JS_FreeValue(ctx, step);
    if (s->cancellation_requested()) { JS_FreeValue(ctx, item); ok = false; break; }
    if (JS_IsException(item)) { ok = false; break; }
    out->values.push_back(item);
  }
  JS_FreeValue(ctx, next); JS_FreeValue(ctx, iterator); return ok;
}
static JSValue named_property(JSContext *ctx, JSValueConst object, const std::string &name) {
  JSAtom atom = JS_NewAtomLen(ctx, name.data(), name.size());
  JSValue value = JS_GetProperty(ctx, object, atom); JS_FreeAtom(ctx, atom); return value;
}
static JSValue wrapper(JSContext *, State *, wi::Ref, Kind, JSValueConst);
static void observe_memory(State *, wi::Memory::Ptr);
static void connect_references(JSContext *, Handle *, Kind);
static JSValue to_js(JSContext *, State *, Type, const wi::Value &);
static JSValue anchor(JSContext *, State *, wi::Ref);
static bool from_js(JSContext *, State *, Type, JSValueConst, wi::Value *);
static JSValue exported_call(JSContext *ctx, JSValueConst, int argc,
                              JSValueConst *argv, int, JSValue *data) {
  Handle *h = handle(ctx, data[0], FunctionKind);
  if (!h) return JS_EXCEPTION;
  State *s = h->state;
  ++s->stats.calls;
  Active active(s, ctx);
  auto func = h->object.As<wi::Func>();
  const auto &type = func->type();
  wi::Values params(type.params.size()), results;
  for (size_t i = 0; i < params.size(); ++i)
    if (!from_js(ctx, s, type.params[i], arg(argc, argv, i), &params[i]))
      return JS_EXCEPTION;
  wi::Trap::Ptr trap;
  if (wabt::Failed(func->Call(s->store, params, results, &trap))) {
    ++s->stats.traps;
    if (JS_HasException(ctx)) return JS_EXCEPTION; // imported JS exception identity
    return wasm_error(ctx, s, RuntimeError,
                      trap ? trap->message().c_str() : "WebAssembly execution failed");
  }
  if (results.empty()) return JS_UNDEFINED;
  if (results.size() == 1) return to_js(ctx, s, type.results[0], results[0]);
  JSValue array = JS_NewArray(ctx);
  for (size_t i = 0; i < results.size(); ++i) {
    JSValue value = to_js(ctx, s, type.results[i], results[i]);
    if (JS_IsException(value) || JS_SetPropertyUint32(ctx, array, i, value) < 0) {
      JS_FreeValue(ctx, array); return JS_EXCEPTION;
    }
  }
  return array;
}
static JSValue wrapper(JSContext *ctx, State *s, wi::Ref ref, Kind kind,
                        JSValueConst new_target = JS_UNDEFINED) {
  auto existing = s->wrappers.find(ref.index);
  if (existing != s->wrappers.end()) return JS_DupValue(ctx, existing->second);
  JSValue proto = JS_UNDEFINED;
  if (!JS_IsUndefined(new_target)) proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  JSValue value = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, classes[kind]) :
                                      JS_NewObjectClass(ctx, classes[kind]);
  JS_FreeValue(ctx, proto);
  if (JS_IsException(value)) return value;
  auto h = new Handle(s, ref);
  JS_SetOpaque(value, h);
  if (kind == MemoryKind) observe_memory(s, h->object.As<wi::Memory>());
  if (kind == FunctionKind) {
    JSValue function = JS_NewCFunctionData(ctx, exported_call,
        h->object.As<wi::Func>()->type().params.size(), 0, 1, &value);
    JS_FreeValue(ctx, value);
    if (JS_IsException(function)) return function;
    value = function;
    std::string name = std::to_string(ref.index);
    JS_DefinePropertyValueStr(ctx, value, "name", JS_NewString(ctx, name.c_str()),
                            JS_PROP_CONFIGURABLE);
  }
  h->registered = true;
  s->wrappers.emplace(ref.index, value);
  if (kind == FunctionKind) {
    h->function_identity = JS_VALUE_GET_PTR(value);
    s->functions.emplace(h->function_identity, ref.index);
  }
  connect_references(ctx, h, kind);
  return value;
}
static JSValue to_js(JSContext *ctx, State *s, Type type, const wi::Value &v) {
  switch (type) {
    case Type::I32: return JS_NewInt32(ctx, v.Get<int32_t>());
    case Type::I64: return JS_NewBigInt64(ctx, v.Get<int64_t>());
    case Type::F32: return JS_NewFloat64(ctx, v.Get<float>());
    case Type::F64: return JS_NewFloat64(ctx, v.Get<double>());
    case Type::FuncRef: {
      wi::Ref ref = v.Get<wi::Ref>();
      return ref == wi::Ref::Null ? JS_NULL : wrapper(ctx, s, ref, FunctionKind);
    }
    case Type::ExternRef: {
      wi::Ref ref = v.Get<wi::Ref>();
      if (ref == wi::Ref::Null) return JS_NULL;
      auto foreign = s->store.UnsafeGet<wi::Foreign>(ref);
      auto binding = static_cast<Binding *>(foreign->ptr());
      return JS_DupValue(ctx, binding->value);
    }
    default: return JS_ThrowTypeError(ctx, "WebAssembly value cannot cross the JavaScript boundary");
  }
}
static bool function_ref(State *s, JSValueConst value, wi::Ref *out) {
  if (!JS_IsObject(value)) return false;
  auto entry = s->functions.find(JS_VALUE_GET_PTR(value));
  if (entry == s->functions.end()) return false;
  *out = wi::Ref(entry->second); return true;
}
static bool from_js(JSContext *ctx, State *s, Type type, JSValueConst value,
                     wi::Value *out) {
  if (s->cancellation_requested()) return false;
  switch (type) {
    case Type::I32: {
      int32_t n; if (JS_ToInt32(ctx, &n, value)) return false;
      *out = wi::Value::Make(n); return true;
    }
    case Type::I64: {
      int64_t n; if (JS_ToBigInt64(ctx, &n, value)) return false;
      *out = wi::Value::Make(n); return true;
    }
    case Type::F32: case Type::F64: {
      double n; if (JS_ToFloat64(ctx, &n, value)) return false;
      *out = type == Type::F32 ? wi::Value::Make(float(n)) : wi::Value::Make(n);
      return true;
    }
    case Type::ExternRef: {
      if (JS_IsNull(value)) { *out = wi::Value::Make(wi::Ref::Null); return true; }
      auto binding = new Binding(s, value);
      auto foreign = wi::Foreign::New(s->store, binding);
      foreign->set_finalizer([binding](wi::Object *) { delete binding; });
      JSValue anchor = wrapper(ctx, s, foreign.ref(), ForeignKind);
      if (JS_IsException(anchor)) return false;
      s->active_roots->push_back(anchor);
      *out = wi::Value::Make(foreign.ref()); return true;
    }
    case Type::FuncRef: {
      wi::Ref ref = wi::Ref::Null;
      if (JS_IsNull(value) || function_ref(s, value, &ref)) {
        if (ref != wi::Ref::Null) s->active_roots->push_back(JS_DupValue(ctx, value));
        *out = wi::Value::Make(ref); return true;
      }
      JS_ThrowTypeError(ctx, "Expected an exported WebAssembly function or null"); return false;
    }
    default:
      JS_ThrowTypeError(ctx, "WebAssembly value cannot cross the JavaScript boundary");
      return false;
  }
}
static bool bytes(JSContext *ctx, State *s, JSValueConst value, std::vector<uint8_t> *out) {
  size_t length = 0;
  uint8_t *data = JS_GetArrayBuffer(ctx, &length, value);
  if (data || !JS_HasException(ctx)) {
    if (length) out->assign(data, data + length);
    return true;
  }
  JS_FreeValue(ctx, JS_GetException(ctx));
  // Typed arrays and DataView both expose an underlying buffer. Access through
  // the engine, not overridable user properties, keeps BufferSource offsets exact.
  size_t offset = 0, element_size = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &element_size);
  if (JS_IsException(buffer)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    buffer = JS_Call(ctx, s->data_view_getters[0], value, 0, nullptr);
    if (JS_IsException(buffer)) return false;
    JSValue start = JS_Call(ctx, s->data_view_getters[1], value, 0, nullptr);
    JSValue size = JS_Call(ctx, s->data_view_getters[2], value, 0, nullptr);
    uint64_t start_index, size_index;
    bool ok = !JS_IsException(start) && !JS_IsException(size) &&
              !JS_ToIndex(ctx, &start_index, start) && !JS_ToIndex(ctx, &size_index, size);
    JS_FreeValue(ctx, start); JS_FreeValue(ctx, size);
    if (!ok) { JS_FreeValue(ctx, buffer); return false; }
    offset = start_index; length = size_index;
  }
  size_t buffer_length = 0;
  data = JS_GetArrayBuffer(ctx, &buffer_length, buffer);
  if ((!data && JS_HasException(ctx)) || offset > buffer_length || length > buffer_length - offset) {
    JS_FreeValue(ctx, buffer); return false;
  }
  if (length) out->assign(data + offset, data + offset + length);
  JS_FreeValue(ctx, buffer); return true;
}
static bool compile(JSContext *ctx, State *s, const std::vector<uint8_t> &data,
                    wi::ModuleDesc *desc, bool quiet) {
  wabt::Errors errors;
  wabt::ReadBinaryOptions options;
  options.features = s->store.features();
  options.fail_on_custom_section_error = false;
  options.interrupt_callback = [s]() { return s->interrupted(); };
  if (wabt::Succeeded(wi::ReadBinaryInterp("WebAssembly", data.data(), data.size(),
                                         options, &errors, desc))) return true;
  if (!quiet) wasm_error(ctx, s, CompileError,
      errors.empty() ? "Invalid WebAssembly binary" : errors.front().message.c_str());
  return false;
}

struct NativeModule {
  TCCState *compiler = nullptr;
  wasm_host_invoke invoke = nullptr;
  ~NativeModule() { if (compiler) tcc_delete(compiler); }
};
struct NativeCompile {
  State *state;
  bool cancelled = false;
  std::string error;
  static int poll(void *opaque) {
    auto *self = static_cast<NativeCompile *>(opaque);
    if (self->state->interrupted())
      self->cancelled = true;
    return self->cancelled;
  }
  static void diagnostic(void *opaque, const char *message) {
    auto *self = static_cast<NativeCompile *>(opaque);
    if (self->error.empty()) self->error = message;
  }
};
static std::shared_ptr<NativeModule> compile_native(JSContext *ctx, State *s,
                                                    const std::vector<uint8_t>& bytes) {
  NativeCompile work{s, false, {}};
  wabt::Module ir;
  wabt::Errors errors;
  wabt::ReadBinaryOptions reader;
  reader.features = s->store.features();
  reader.fail_on_custom_section_error = false;
  reader.interrupt_callback = [&]() { return NativeCompile::poll(&work) != 0; };
  bool ok = wabt::Succeeded(wabt::ReadBinaryIr("WebAssembly native", bytes, reader, &errors, &ir));
  if (!ok && !errors.empty()) work.error = errors.front().message;
  // The current wasm2c exception runtime still uses an incompatible fixed
  // exception-payload ABI. Never compile it as if it shared the Store's tags.
  if (ok && ir.features_used.exceptions) {
    ok = false; work.error = "wasm2c native exception payload/tag runtime ABI is not supported";
  }
  if (ok) ok = wabt::Succeeded(wabt::GenerateNames(&ir)) && wabt::Succeeded(wabt::ApplyNames(&ir));
  wabt::MemoryStream source;
  if (ok) {
    wabt::WriteCOptions writer;
    writer.module_name = "browser";
    writer.features = reader.features;
    writer.host_resources = true;
    writer.interrupt_callback = reader.interrupt_callback;
    writer.host_diagnostic = [&](const char *message) { NativeCompile::diagnostic(&work, message); };
    ok = wabt::Succeeded(wabt::WriteC({&source}, &source, &source, "", "", &ir, writer));
  }
  auto native = std::make_shared<NativeModule>();
  if (ok) {
    const auto& output = source.output_buffer().data;
    std::string text(reinterpret_cast<const char*>(output.data()), output.size());
    int compiled = wasm_rt_compile_tcc(&native->compiler, text.c_str(),
        NativeCompile::diagnostic, NativeCompile::poll, &work);
    if (compiled == -2) { JS_ThrowOutOfMemory(ctx); return {}; }
    ok = compiled == 0;
    if (ok) {
      native->invoke = reinterpret_cast<wasm_host_invoke>(tcc_get_symbol(native->compiler, "wasm_host_invoke_entry"));
      ok = native->invoke != nullptr;
    }
  }
  if (work.cancelled) {
    if (!JS_HasException(ctx)) wasm_error(ctx, s, CompileError, "WebAssembly native compilation interrupted");
    return {};
  }
  if (!ok) {
    if (JS_HasException(ctx)) return {};
    ++s->stats.native_fallbacks;
    s->native_fallback = work.error.empty() ? "wasm2c/TCC could not compile or relocate this module" : work.error;
    return {};
  }
  ++s->stats.native_modules;
  return native;
}
static wasm_host_value host_value(Type type, const wi::Value& value) {
  wasm_host_value result = {};
  if (type.IsRef()) result.ref = value.Get<wi::Ref>().index;
  else switch (type) {
    case Type::I32: result.i32 = value.Get<uint32_t>(); break;
    case Type::I64: result.i64 = value.Get<uint64_t>(); break;
    case Type::F32: result.f32 = value.Get<float>(); break;
    case Type::F64: result.f64 = value.Get<double>(); break;
    case Type::V128: { auto v = value.Get<::v128>(); memcpy(result.v128, &v, sizeof(v)); break; }
    default: break;
  }
  return result;
}
static wi::Value store_value(Type type, const wasm_host_value& value) {
  if (type.IsRef()) return wi::Value::Make(wi::Ref{static_cast<wabt::Index>(value.ref)});
  switch (type) {
    case Type::I32: return wi::Value::Make(value.i32);
    case Type::I64: return wi::Value::Make(value.i64);
    case Type::F32: return wi::Value::Make(value.f32);
    case Type::F64: return wi::Value::Make(value.f64);
    case Type::V128: { ::v128 v; memcpy(&v, value.v128, sizeof(v)); return wi::Value::Make(v); }
    default: return {};
  }
}

// Native reference locals are real C stack slots. Before a host re-entry can
// allocate JS values, temporarily root their existing JS anchors. Registration
// itself is a pair of pointer stores in generated code, not a per-op syscall.
struct NativeRoots {
  explicit NativeRoots(State *s) {
    std::map<size_t, JSValue> current;
    auto retain = [&](size_t ref) {
      if (current.find(ref) != current.end()) return;
      auto found = s->wrappers.find(ref);
      if (found != s->wrappers.end()) current.emplace(ref, JS_DupValueRT(s->rt, found->second));
    };
    for (auto *frame = s->native_roots; frame; frame = frame->previous) {
      for (size_t i = 0; i < frame->count; ++i) {
        size_t ref = *frame->slots[i];
        retain(ref);
      }
    }
    for (auto *thread : s->store.threads()) if (thread->execution_owner() == s->task)
      thread->VisitRoots([&](wi::Ref ref) { retain(ref.index); });
    current.swap(s->native_anchors);
    // Retain the current roots beyond this callback: the author may remove the
    // table/global/Instance edge while its reference is still in a native local.
    // Acquire the new graph before releasing old roots, without allocating JS.
    for (const auto& item : current) JS_FreeValueRT(s->rt, item.second);
  }
};
static void refresh_execution_roots(State *s) { NativeRoots roots(s); }

struct NativeInstance : std::enable_shared_from_this<NativeInstance> {
  State *state;
  wi::Ref instance_ref;
  std::shared_ptr<NativeModule> module;
  std::vector<wasm_rt_memory_t> memory_views;
  wasm_host_instance host = {};
  std::string last_trap;
  NativeInstance(State *s, wi::Instance::Ptr instance, std::shared_ptr<NativeModule> m)
      : state(s), instance_ref(instance.ref()), module(std::move(m)), memory_views(instance->memories().size()) {
    host.opaque = this; host.root_head = &s->native_roots;
    host.owner_ref = instance.ref().index;
    host.memory = memory; host.memory_grow = memory_grow; host.memory_init = memory_init;
    host.data_drop = data_drop; host.global = global; host.table = table;
    host.call = call; host.indirect = indirect; host.func_ref = func_ref;
  }
  wi::Instance::Ptr instance() { return wi::Instance::Ptr(state->store, instance_ref); }
  static NativeInstance *self(void *p) { return static_cast<NativeInstance *>(p); }
  static int poll(void *p) {
    State *s = self(p)->state;
    return s->interrupted();
  }
  static wasm_rt_memory_t *memory(void *p, uint32_t index) {
    auto *n = self(p); auto inst = n->instance();
    auto mem = n->state->store.UnsafeGet<wi::Memory>(inst->memories()[index]);
    auto& view = n->memory_views[index];
    view.data = mem->UnsafeData(); view.size = mem->ByteSize();
    view.data_end = view.data ? view.data + view.size : nullptr;
    view.page_size = mem->type().page_size; view.pages = mem->PageSize();
    view.max_pages = mem->type().limits.has_max ? mem->type().limits.max :
        (mem->type().limits.is_64 ? UINT64_MAX / view.page_size : (uint64_t(UINT32_MAX) + 1) / view.page_size);
    view.is64 = mem->type().limits.is_64;
    return &view;
  }
  static uint64_t memory_grow(void *p, uint32_t index, uint64_t pages) {
    auto *n = self(p); NativeRoots roots(n->state); auto inst = n->instance();
    auto mem = n->state->store.UnsafeGet<wi::Memory>(inst->memories()[index]);
    uint64_t old = mem->PageSize();
    return wabt::Succeeded(mem->Grow(pages)) ? old : UINT64_MAX;
  }
  static int memory_init(void *p, uint32_t index, uint32_t segment, uint64_t d, uint64_t s, uint64_t count) {
    auto *n = self(p); auto inst = n->instance();
    auto mem = n->state->store.UnsafeGet<wi::Memory>(inst->memories()[index]);
    return wabt::Succeeded(mem->Init(d, inst->datas()[segment], s, count)) ? 0 : WASM_RT_TRAP_OOB;
  }
  static void data_drop(void *p, uint32_t segment) { self(p)->instance()->datas()[segment].Drop(); }
  static int global(void *p, uint32_t index, int set, wasm_host_value *value) {
    auto *n = self(p); NativeRoots roots(n->state); auto inst = n->instance();
    auto object = n->state->store.UnsafeGet<wi::Global>(inst->globals()[index]);
    Type type = object->type().type;
    if (set) {
      if (type.IsRef()) object->Set(n->state->store, wi::Ref{static_cast<wabt::Index>(value->ref)});
      else object->UnsafeSet(store_value(type, *value));
    } else *value = host_value(type, object->Get());
    return JS_HasException(n->state->active) ? WASM_RT_TRAP_UNREACHABLE : 0;
  }
  static int table(void *p, wasm_host_table_op op, uint32_t dest, uint32_t src,
                   uint64_t a, uint64_t b, uint64_t c, wasm_host_value *value) {
    auto *n = self(p); NativeRoots roots(n->state); auto inst = n->instance();
    if (op == WASM_HOST_ELEM_DROP) { inst->elems()[dest].Drop(); return 0; }
    auto object = n->state->store.UnsafeGet<wi::Table>(inst->tables()[dest]);
    wi::Ref ref{static_cast<wabt::Index>(value->ref)};
    Result result = Result::Ok;
    switch (op) {
      case WASM_HOST_TABLE_GET:
        result = object->Get(a, &ref); value->ref = ref.index; break;
      case WASM_HOST_TABLE_SET: result = object->Set(n->state->store, a, ref); break;
      case WASM_HOST_TABLE_SIZE: value->i64 = object->elements().size(); break;
      case WASM_HOST_TABLE_GROW: {
        uint64_t old = object->elements().size();
        value->i64 = wabt::Succeeded(object->Grow(n->state->store, a, ref)) ? old : UINT64_MAX; break;
      }
      case WASM_HOST_TABLE_FILL: result = object->Fill(n->state->store, a, ref, b); break;
      case WASM_HOST_TABLE_COPY: {
        auto source = n->state->store.UnsafeGet<wi::Table>(inst->tables()[src]);
        result = wi::Table::Copy(n->state->store, *object, a, *source, b, c); break;
      }
      case WASM_HOST_TABLE_INIT:
        if (b > UINT32_MAX || c > UINT32_MAX) return WASM_RT_TRAP_OOB;
        result = object->Init(n->state->store, a, inst->elems()[src], b, c); break;
      default: break;
    }
    if (JS_HasException(n->state->active)) return WASM_RT_TRAP_UNREACHABLE;
    return wabt::Succeeded(result) ? 0 : WASM_RT_TRAP_OOB;
  }
  static wasm_host_ref func_ref(void *p, uint32_t index) { return self(p)->instance()->funcs()[index].index; }
  int call_ref(wi::Ref ref, const wasm_host_value *params, wasm_host_value *results) {
    NativeRoots roots(state);
    auto function = state->store.UnsafeGet<wi::Func>(ref);
    const auto& type = function->type();
    wi::Values args(type.params.size()), returns;
    for (size_t i = 0; i < args.size(); ++i) args[i] = store_value(type.params[i], params[i]);
    wi::Trap::Ptr trap;
    if (wabt::Failed(function->Call(state->store, args, returns, &trap))) {
      last_trap = trap ? trap->message() : "WebAssembly import failed";
      return WASM_RT_TRAP_UNREACHABLE;
    }
    for (size_t i = 0; i < type.results.size(); ++i) results[i] = host_value(type.results[i], returns[i]);
    return 0;
  }
  static int call(void *p, uint32_t index, const wasm_host_value *params, wasm_host_value *results) {
    auto *n = self(p); return n->call_ref(n->instance()->funcs()[index], params, results);
  }
  static int indirect(void *p, uint32_t type, uint32_t table_index, uint64_t index,
                      const wasm_host_value *params, wasm_host_value *results) {
    auto *n = self(p); auto inst = n->instance();
    auto table = n->state->store.UnsafeGet<wi::Table>(inst->tables()[table_index]);
    wi::Ref ref;
    if (wabt::Failed(table->Get(index, &ref)) || ref == wi::Ref::Null) return WASM_RT_TRAP_CALL_INDIRECT;
    auto func = n->state->store.UnsafeGet<wi::Func>(ref);
    auto mod = n->state->store.UnsafeGet<wi::Module>(inst->module());
    if (wabt::Failed(Match(func->type(), mod->desc().func_types[type], nullptr))) return WASM_RT_TRAP_CALL_INDIRECT;
    return n->call_ref(ref, params, results);
  }
  Result invoke(uint32_t index, const wi::Values& params, wi::Values& results, wi::Trap::Ptr *trap) {
    auto inst = instance(); auto function = state->store.UnsafeGet<wi::Func>(inst->funcs()[index]);
    const auto& type = function->type();
    std::vector<wasm_host_value> args(params.size()), returns(type.results.size());
    for (size_t i = 0; i < params.size(); ++i) args[i] = host_value(type.params[i], params[i]);
    struct Invocation { NativeInstance *self; uint32_t index; const wasm_host_value *args; wasm_host_value *returns; } invocation{this,index,args.data(),returns.data()};
    auto entry = +[](void *opaque) {
      auto *call = static_cast<Invocation *>(opaque);
      wasm_rt_native_invoke(call->self->module->invoke, &call->self->host,
                            call->index, call->args, call->returns);
    };
    wasm_rt_poll_state previous;
    if (wasm_rt_set_poll(poll, this, &previous) < 0) {
      JS_ThrowOutOfMemory(state->active); *trap = wi::Trap::New(state->store, "Native runtime allocation failed"); return Result::Error;
    }
    auto *saved_roots = state->native_roots;
    last_trap.clear(); ++state->stats.native_calls;
    int status = wasm_rt_run(entry, &invocation);
    state->native_roots = saved_roots;
    // The callee's C root frame has gone, but its reference results have not
    // reached a caller slot yet. Transfer their JS ownership before pruning
    // the last snapshot (the author may have cleared the originating global).
    if (!status) for (size_t i = 0; i < type.results.size(); ++i) {
      if (!type.results[i].IsRef()) continue;
      JSValue root = anchor(state->active, state, wi::Ref{static_cast<wabt::Index>(returns[i].ref)});
      if (JS_IsException(root)) { status = WASM_RT_TRAP_UNREACHABLE; break; }
      state->active_roots->push_back(root);
    }
    NativeRoots restored_roots(state);
    wasm_rt_set_poll(previous.callback, previous.opaque, nullptr);
    if (status) {
      const char *message = last_trap.empty() ? wasm_rt_strerror(static_cast<wasm_rt_trap_t>(status)) : last_trap.c_str();
      *trap = wi::Trap::New(state->store, message); return Result::Error;
    }
    results.resize(type.results.size());
    for (size_t i = 0; i < results.size(); ++i) results[i] = store_value(type.results[i], returns[i]);
    return Result::Ok;
  }
};

static void bind_native(State *s, wi::Instance::Ptr instance, std::shared_ptr<NativeModule> module) {
  auto native = std::make_shared<NativeInstance>(s, instance, std::move(module));
  auto owner = new std::shared_ptr<NativeInstance>(native);
  instance->set_host_info(owner);
  instance->set_finalizer([owner](wi::Object *) { delete owner; });
  std::weak_ptr<NativeInstance> weak = native;
  auto mod = s->store.UnsafeGet<wi::Module>(instance->module());
  size_t imports = instance->funcs().size() - mod->desc().funcs.size();
  for (size_t i = imports; i < instance->funcs().size(); ++i) {
    auto func = s->store.UnsafeGet<wi::DefinedFunc>(instance->funcs()[i]);
    func->set_native_callback([weak, i](wi::Thread& thread, const wi::Values& params,
                                       wi::Values& results, wi::Trap::Ptr *trap) -> Result {
      auto live = weak.lock();
      if (!live) { *trap = wi::Trap::New(thread.store(), "Native instance lifetime ended"); return Result::Error; }
      return live->invoke(i, params, results, trap);
    });
  }
}

struct ExecutionFrame {
  JSContext *active = nullptr;
  std::vector<JSValue> *roots = nullptr;
  Active *active_frame = nullptr;
  wasm_host_roots *native_roots = nullptr;
  std::map<size_t, JSValue> native_anchors;
  PromiseTask *task = nullptr;
  void swap(State *s) {
    std::swap(active, s->active); std::swap(roots, s->active_roots);
    std::swap(active_frame, s->active_frame); std::swap(native_roots, s->native_roots);
    native_anchors.swap(s->native_anchors); std::swap(task, s->task);
  }
  static void mark_frame(JSRuntime *rt, Active *frame,
                         const std::map<size_t, JSValue>& anchors, JS_MarkFunc *fn) {
    for (; frame; frame = frame->previous)
      for (auto value : frame->roots) JS_MarkValue(rt, value, fn);
    for (const auto& item : anchors) JS_MarkValue(rt, item.second, fn);
  }
};

struct PromiseTask {
  std::shared_ptr<State> lifetime;
  State *state;
  JSRuntime *rt;
  JSContext *ctx;
  wasm_rt_coroutine *coroutine = nullptr;
  uintptr_t low = 0, high = 0;
  JSExecutionStackState js_stack = {};
  ExecutionFrame frame;
  JSValue self = JS_UNDEFINED; // weak; callers and Promise reactions own it
  JSValue owner, function;
  JSValue promise = JS_UNDEFINED, resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
  JSValue awaited = JS_UNDEFINED, resumed = JS_UNDEFINED, result = JS_UNDEFINED;
  std::vector<JSValue> arguments;
  bool running = false, started = false, done = false, cancelled = false;
  bool allow_suspend = false, rejected = false, result_rejected = false;
  bool registered = false;
  PromiseTask(State *s, JSContext *c, JSValueConst f, int argc, JSValueConst *argv)
      : lifetime(s->shared_from_this()), state(s), rt(s->rt), ctx(JS_DupContext(c)),
        owner(JS_DupValue(c, s->owner)), function(JS_DupValue(c, f)) {
    frame.task = this;
    JS_InitExecutionStackState(rt, &js_stack, 0, 0);
    for (int i = 0; i < argc; ++i) arguments.push_back(JS_DupValue(c, argv[i]));
    s->tasks.push_back(this); registered = true; ++s->stats.jspi_pending;
  }
  void release(JSValue& value) {
    JSValue old = value; value = JS_UNDEFINED; JS_FreeValueRT(rt, old);
  }
  void cleanup() {
    if (coroutine) { wasm_rt_coroutine_destroy(coroutine); coroutine = nullptr; }
    JS_FreeExecutionStackState(rt, &js_stack);
    for (auto value : arguments) JS_FreeValueRT(rt, value);
    arguments.clear();
    release(owner); release(function); release(promise); release(resolve); release(reject);
    release(awaited); release(resumed); release(result);
    for (const auto& item : frame.native_anchors) JS_FreeValueRT(rt, item.second);
    frame.native_anchors.clear();
    frame.active_frame = nullptr; frame.roots = nullptr; frame.native_roots = nullptr;
    if (registered) {
      auto& tasks = state->tasks;
      for (auto i = tasks.begin(); i != tasks.end(); ++i) if (*i == this) { tasks.erase(i); break; }
      registered = false; --state->stats.jspi_pending;
    }
    if (ctx) { JSContext *old = ctx; ctx = nullptr; JS_FreeContext(old); }
    state = nullptr; lifetime.reset();
  }
  ~PromiseTask() { cancel(); cleanup(); }
  static void entry(void *opaque) {
    auto *task = static_cast<PromiseTask *>(opaque);
    State *s = task->state; JSContext *ctx = task->ctx;
    Active active(s, ctx);
    wi::Ref ref;
    if (!function_ref(s, task->function, &ref)) {
      JS_ThrowTypeError(ctx, "promising requires an exported WebAssembly function");
    } else {
      auto function = s->store.UnsafeGet<wi::Func>(ref);
      const auto& type = function->type();
      wi::Values params(type.params.size()), results;
      bool valid = true;
      for (size_t i = 0; valid && i < params.size(); ++i)
        valid = from_js(ctx, s, type.params[i], i < task->arguments.size() ? task->arguments[i] : JS_UNDEFINED, &params[i]);
      if (valid) {
        task->allow_suspend = true;
        ++s->stats.calls;
        wi::Trap::Ptr trap;
        Result called = function->Call(s->store, params, results, &trap);
        task->allow_suspend = false;
        if (!task->cancelled && wabt::Failed(called)) {
          ++s->stats.traps;
          if (!JS_HasException(ctx)) wasm_error(ctx, s, RuntimeError, trap ? trap->message().c_str() : "WebAssembly execution failed");
        } else if (!task->cancelled) {
          if (results.size() == 1) task->result = to_js(ctx, s, type.results[0], results[0]);
          else if (results.size() > 1) {
            task->result = JS_NewArray(ctx);
            for (size_t i = 0; !JS_IsException(task->result) && i < results.size(); ++i) {
              JSValue value = to_js(ctx, s, type.results[i], results[i]);
              if (JS_IsException(value) || JS_SetPropertyUint32(ctx, task->result, i, value) < 0) {
                task->release(task->result); task->result = JS_EXCEPTION; break;
              }
            }
          }
        }
      }
    }
    if (JS_HasException(ctx)) {
      task->release(task->result); task->result = JS_GetException(ctx); task->result_rejected = true;
    }
    task->done = true;
  }
  void resume() {
    if (done || running) return;
    if (!coroutine) {
      coroutine = wasm_rt_coroutine_create(entry, this);
      if (!coroutine || wasm_rt_coroutine_bounds(coroutine, &low, &high) < 0) {
        JS_ThrowOutOfMemory(ctx); result = JS_GetException(ctx); result_rejected = true; done = true;
      } else JS_InitExecutionStackState(rt, &js_stack, low, high);
    }
    if (!done) {
      running = true;
      if (started) ++state->stats.jspi_resumptions;
      started = true;
      frame.swap(state); JS_SwapExecutionStackState(rt, &js_stack);
      int status = wasm_rt_coroutine_resume(coroutine);
      JS_SwapExecutionStackState(rt, &js_stack); frame.swap(state);
      running = false;
      if (status == WASM_RT_COROUTINE_ERROR) {
        JS_ThrowInternalError(ctx, "WebAssembly continuation could not resume");
        result = JS_GetException(ctx); result_rejected = true; done = true;
      }
    }
    if (done) {
      // Promise resolution may synchronously read an author-defined then
      // getter. Retirement from that getter must not free this live frame.
      running = true;
      if (!cancelled) {
        JSValue ignored = JS_Call(ctx, result_rejected ? reject : resolve, JS_UNDEFINED, 1, &result);
        if (JS_IsException(ignored)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
        JS_FreeValue(ctx, ignored);
      }
      running = false;
      cleanup();
    }
  }
  void cancel() {
    cancelled = true;
    if (done) return;
    if (running) return;
    if (!started) { done = true; return; }
    resume();
  }
  static JSValue reaction(JSContext *, JSValueConst, int argc, JSValueConst *argv, int magic, JSValue *data) {
    auto *task = static_cast<PromiseTask *>(JS_GetOpaque(data[0], classes[PromiseTaskKind]));
    if (!task || task->done || task->cancelled) return JS_UNDEFINED;
    task->resumed = JS_DupValueRT(task->rt, arg(argc, argv, 0));
    task->rejected = magic != 0; task->resume();
    return JS_UNDEFINED;
  }
  JSValue await(JSValue value) {
    if (cancelled || state->retired) { JS_FreeValue(ctx, value); return JS_EXCEPTION; }
    JSValue normalized = JS_PromiseResolveIntrinsic(ctx, value); JS_FreeValue(ctx, value);
    if (cancelled || state->retired) { JS_FreeValue(ctx, normalized); return JS_EXCEPTION; }
    if (JS_IsException(normalized)) return normalized;
    awaited = normalized;
    JSValue callbacks[] = {
      JS_NewCFunctionData(ctx, reaction, 1, 0, 1, &self),
      JS_NewCFunctionData(ctx, reaction, 1, 1, 1, &self)
    };
    int added = JS_IsException(callbacks[0]) || JS_IsException(callbacks[1]) ? -1 :
        JS_PromiseAddReactions(ctx, awaited, callbacks[0], callbacks[1]);
    for (auto callback : callbacks) JS_FreeValue(ctx, callback);
    if (added < 0) { release(awaited); return JS_EXCEPTION; }
    NativeRoots protected_roots(state);
    ++state->stats.jspi_suspensions;
    if (wasm_rt_coroutine_yield(coroutine) < 0) {
      release(awaited); return JS_ThrowInternalError(ctx, "WebAssembly continuation could not suspend");
    }
    release(awaited);
    if (cancelled) return JS_EXCEPTION; // uncatchable Wasm trap, no author callbacks
    JSValue returned = resumed; resumed = JS_UNDEFINED;
    return rejected ? JS_Throw(ctx, returned) : returned;
  }
};

bool State::cancellation_requested() const {
  return retired || (task && task->cancelled);
}
bool State::interrupted() {
  if (cancellation_requested()) return true;
  bool requested = active && poll && poll(active);
  return requested || cancellation_requested();
}

static void task_finalize(PromiseTask *task) { delete task; }
static void task_mark(JSRuntime *rt, PromiseTask *task, JS_MarkFunc *fn) {
  if (!task) return;
  if (task->ctx) JS_MarkContextRef(rt, task->ctx, fn);
  for (auto value : {task->owner, task->function, task->promise, task->resolve,
                      task->reject, task->awaited, task->resumed, task->result}) JS_MarkValue(rt, value, fn);
  for (auto value : task->arguments) JS_MarkValue(rt, value, fn);
  if (task->state && task->state->task == task)
    ExecutionFrame::mark_frame(rt, task->state->active_frame, task->state->native_anchors, fn);
  else {
    JS_MarkValue(rt, task->js_stack.exception, fn);
    ExecutionFrame::mark_frame(rt, task->frame.active_frame, task->frame.native_anchors, fn);
  }
}

static JSValue promising_call(JSContext *ctx, JSValueConst, int argc,
                               JSValueConst *argv, int, JSValue *data) {
  State *s = get_state(data[0]);
  JSValue object = JS_NewObjectClass(ctx, classes[PromiseTaskKind]);
  if (JS_IsException(object)) return object;
  auto *task = new PromiseTask(s, ctx, data[1], argc, argv);
  JS_SetOpaque(object, task); task->self = object;
  JSValue resolving[2];
  task->promise = JS_NewPromiseCapability(ctx, resolving);
  if (JS_IsException(task->promise)) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
  task->resolve = resolving[0]; task->reject = resolving[1];
  JSValue promise = JS_DupValue(ctx, task->promise);
  if (s->retired) task->cancel(); else task->resume();
  JS_FreeValue(ctx, object);
  return promise;
}
static JSValue promising(JSContext *ctx, JSValueConst, int argc,
                          JSValueConst *argv, int, JSValue *data) {
  State *s = get_state(data[0]); wi::Ref ref;
  JSValue function = arg(argc, argv, 0);
  if (!function_ref(s, function, &ref)) return JS_ThrowTypeError(ctx, "promising requires an exported WebAssembly function");
  JSValue captured[] = {data[0], function};
  return JS_NewCFunctionData(ctx, promising_call, 1, 0, 2, captured);
}
static JSValue suspending_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                JSValueConst *argv, int, JSValue *) {
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Suspending requires new");
  JSValue function = arg(argc, argv, 0);
  if (!JS_IsFunction(ctx, function)) return JS_ThrowTypeError(ctx, "Suspending requires a callable");
  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(proto)) return proto;
  JSValue object = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, classes[SuspendingKind]) : JS_NewObjectClass(ctx, classes[SuspendingKind]);
  JS_FreeValue(ctx, proto);
  if (!JS_IsException(object)) JS_SetOpaque(object, new SuspendingData{JS_DupValue(ctx, function)});
  return object;
}

struct CompileOptions {
  bool js_string = false;
  bool has_constants = false;
  std::string constants;
};
static bool compile_options(JSContext *ctx, State *s, JSValueConst options,
                              CompileOptions *out, bool quiet = false) {
  if (JS_IsUndefined(options) || JS_IsNull(options)) return true;
  if (!JS_IsObject(options)) {
    JS_ThrowTypeError(ctx, "WebAssembly compile options must be a dictionary"); return false;
  }
  JSValue builtins = JS_GetPropertyStr(ctx, options, "builtins");
  JSValue strings = JS_GetPropertyStr(ctx, options, "importedStringConstants");
  if (JS_IsException(builtins) || JS_IsException(strings)) {
    JS_FreeValue(ctx, builtins); JS_FreeValue(ctx, strings); return false;
  }
  if (!JS_IsUndefined(strings) && !JS_IsNull(strings)) {
    const char *name = JS_ToCString(ctx, strings);
    if (!name) { JS_FreeValue(ctx, builtins); JS_FreeValue(ctx, strings); return false; }
    out->has_constants = true; out->constants = name; JS_FreeCString(ctx, name);
  }
  bool duplicate = false;
  if (!JS_IsUndefined(builtins)) {
    if (!JS_IsObject(builtins)) {
      JS_FreeValue(ctx, builtins); JS_FreeValue(ctx, strings);
      JS_ThrowTypeError(ctx, "WebAssembly builtins must be a sequence"); return false;
    }
    ValueList values(ctx);
    std::vector<std::string> names;
    if (iterable_values(ctx, s, builtins, &values)) {
      for (JSValue value : values.values) {
        size_t name_length;
        const char *name = JS_ToCStringLen(ctx, &name_length, value);
        if (!name) break;
        std::string full_name(name, name_length);
        for (const auto &existing : names) if (existing == full_name) duplicate = true;
        names.push_back(full_name);
        if (full_name == "js-string") out->js_string = true;
        JS_FreeCString(ctx, name);
      }
    }
  }
  JS_FreeValue(ctx, builtins); JS_FreeValue(ctx, strings);
  if (JS_HasException(ctx)) return false;
  if (duplicate) {
    if (!quiet) wasm_error(ctx, s, CompileError, "WebAssembly builtin set names must be unique");
    return false;
  }
  return true;
}
static bool supported_options(JSContext *ctx, State *s, const wi::ModuleDesc &desc,
                                const CompileOptions &options, bool quiet) {
  for (const auto &entry : desc.imports) {
    const auto &module = entry.type.module;
    if ((options.js_string && module == "wasm:js-string") ||
        (options.has_constants && module == options.constants)) {
      if (!quiet) wasm_error(ctx, s, CompileError,
          "WebAssembly string builtins are not supported by this interpreter");
      return false;
    }
  }
  return true;
}
static JSValue module_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv, int, JSValue *data) {
  State *s = get_state(data[0]); Active active(s, ctx);
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Module requires new");
  CompileOptions options;
  if (!compile_options(ctx, s, arg(argc, argv, 1), &options)) return JS_EXCEPTION;
  std::vector<uint8_t> binary;
  if (!bytes(ctx, s, arg(argc, argv, 0), &binary)) return JS_EXCEPTION;
  // All byte-compilation APIs use this native constructor, including the
  // captured Promise/streaming facade. validate() intentionally does not use
  // this executable-code authorization gate; neither does Instance(Module).
  if (s->can_compile) {
    int allowed = s->can_compile(ctx);
    if (allowed < 0) {
      if (JS_HasException(ctx)) return JS_EXCEPTION;
      return JS_ThrowInternalError(ctx, "WebAssembly authorization callback failed without an exception");
    }
    if (!allowed) return wasm_error(ctx, s, CompileError,
                                    "WebAssembly compilation is blocked by Content Security Policy");
  }
  wi::ModuleDesc desc;
  if (!compile(ctx, s, binary, &desc, false)) return JS_EXCEPTION;
  if (!supported_options(ctx, s, desc, options, false)) return JS_EXCEPTION;
  auto native = compile_native(ctx, s, binary);
  if (JS_HasException(ctx)) return JS_EXCEPTION;
  ++s->stats.compiled_modules;
  s->stats.compiled_bytes += binary.size();
  auto mod = wi::Module::New(s->store, std::move(desc));
  if (native) {
    auto owner = new std::shared_ptr<NativeModule>(std::move(native));
    mod->set_host_info(owner);
    mod->set_finalizer([owner](wi::Object *) { delete owner; });
  }
  JSValue value = wrapper(ctx, s, mod.ref(), ModuleKind, new_target);
  if (!JS_IsException(value)) handle(ctx, value, ModuleKind)->binary = std::move(binary);
  return value;
}
static JSValue validate(JSContext *ctx, JSValueConst, int argc,
                         JSValueConst *argv, int, JSValue *data) {
  State *s = get_state(data[0]); Active active(s, ctx);
  std::vector<uint8_t> binary;
  if (!bytes(ctx, s, arg(argc, argv, 0), &binary)) return JS_EXCEPTION;
  CompileOptions options;
  if (!compile_options(ctx, s, arg(argc, argv, 1), &options, true))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
  wi::ModuleDesc desc;
  return JS_NewBool(ctx, compile(ctx, s, binary, &desc, true) &&
                        supported_options(ctx, s, desc, options, true));
}
static JSValue module_types(JSContext *ctx, JSValueConst, int argc,
                             JSValueConst *argv, int imports, JSValue *) {
  Handle *h = handle(ctx, arg(argc, argv, 0), ModuleKind);
  if (!h) return JS_EXCEPTION;
  auto mod = h->object.As<wi::Module>();
  JSValue array = JS_NewArray(ctx);
  size_t count = imports ? mod->import_types().size() : mod->export_types().size();
  for (size_t i = 0; i < count; ++i) {
    JSValue item = JS_NewObject(ctx);
    const auto &name = imports ? mod->import_types()[i].name : mod->export_types()[i].name;
    auto kind = imports ? mod->import_types()[i].type->kind : mod->export_types()[i].type->kind;
    const char *kind_name = wi::GetName(kind);
    if (kind == wi::ExternKind::Func) kind_name = "function";
    JS_SetPropertyStr(ctx, item, "name", JS_NewStringLen(ctx, name.data(), name.size()));
    JS_SetPropertyStr(ctx, item, "kind", JS_NewString(ctx, kind_name));
    if (imports) {
      const auto &module = mod->import_types()[i].module;
      JS_SetPropertyStr(ctx, item, "module", JS_NewStringLen(ctx, module.data(), module.size()));
    }
    JS_SetPropertyUint32(ctx, array, i, item);
  }
  return array;
}
static bool read_leb(const std::vector<uint8_t> &b, size_t *pos, uint32_t *value) {
  uint64_t n = 0;
  // u32 encoding occupies at most ceil(32 / 7) bytes in the core binary format.
  for (unsigned shift = 0; shift < 35 && *pos < b.size(); shift += 7) {
    uint8_t c = b[(*pos)++]; n |= uint64_t(c & 0x7f) << shift;
    if (!(c & 0x80)) { if (n > UINT32_MAX) return false; *value = n; return true; }
  }
  return false;
}
static JSValue custom_sections(JSContext *ctx, JSValueConst, int argc,
                                JSValueConst *argv, int, JSValue *) {
  Handle *h = handle(ctx, arg(argc, argv, 0), ModuleKind);
  if (!h) return JS_EXCEPTION;
  size_t name_length;
  const char *name = JS_ToCStringLen(ctx, &name_length, arg(argc, argv, 1));
  if (!name) return JS_EXCEPTION;
  JSValue array = JS_NewArray(ctx);
  const auto &b = h->binary;
  size_t pos = 8; // four-byte magic followed by four-byte binary version
  uint32_t index = 0;
  while (pos < b.size()) {
    uint8_t id = b[pos++]; uint32_t size;
    if (!read_leb(b, &pos, &size) || size > b.size() - pos) break;
    size_t end = pos + size;
    if (!id) {
      uint32_t length;
      if (!read_leb(b, &pos, &length) || length > end - pos) break;
      if (length == name_length && !memcmp(b.data() + pos, name, length)) {
        pos += length;
        JS_SetPropertyUint32(ctx, array, index++, JS_NewArrayBufferCopy(ctx,
                                                 b.data() + pos, end - pos));
      }
    }
    pos = end;
  }
  JS_FreeCString(ctx, name); return array;
}
static wi::HostFunc::Ptr host_func(State *s, JSValueConst callable,
                                  const wi::FuncType &type, bool suspending = false) {
  auto binding = new Binding(s, callable);
  auto func = wi::HostFunc::New(s->store, type,
      [s, binding, type, suspending](wi::Thread &, const wi::Values &params,
                        wi::Values &results, wi::Trap::Ptr *trap) {
    JSContext *ctx = s->active;
    PromiseTask *task = s->task;
    if (s->cancellation_requested()) {
      *trap = wi::Trap::New(s->store, "WebAssembly execution cancelled"); return Result::Error;
    }
    if (suspending && (!task || !task->allow_suspend || task->cancelled || JS_HasActiveJSFrames(s->rt))) {
      wasm_error(ctx, s, SuspendError, "WebAssembly suspension requires an active promising call without intervening JavaScript frames");
      *trap = wi::Trap::New(s->store, "WebAssembly suspension is not permitted"); return Result::Error;
    }
    NativeRoots protected_roots(s);
    if (task && JS_RebaseExecutionStack(s->rt, task->low, task->high) < 0) {
      JS_ThrowInternalError(ctx, "WebAssembly callback stack bounds are invalid");
      *trap = wi::Trap::New(s->store, "Invalid callback stack bounds"); return Result::Error;
    }
    std::vector<JSValue> args;
    for (size_t i = 0; i < params.size(); ++i) {
      JSValue value = to_js(ctx, s, type.params[i], params[i]);
      if (JS_IsException(value)) break;
      args.push_back(value);
    }
    JSValue result = args.size() == params.size() ?
        JS_Call(ctx, binding->value, JS_UNDEFINED, args.size(), args.data()) : JS_EXCEPTION;
    for (JSValue value : args) JS_FreeValue(ctx, value);
    if (s->cancellation_requested()) {
      JS_FreeValue(ctx, result);
      *trap = wi::Trap::New(s->store, "WebAssembly execution cancelled"); return Result::Error;
    }
    if (suspending && !JS_IsException(result)) result = task->await(result);
    bool ok = !JS_IsException(result);
    results.resize(type.results.size());
    if (ok && type.results.size() == 1)
      ok = from_js(ctx, s, type.results[0], result, &results[0]);
    if (ok && type.results.size() > 1) {
      ValueList values(ctx);
      ok = iterable_values(ctx, s, result, &values);
      if (ok && values.values.size() != type.results.size()) {
        JS_ThrowTypeError(ctx, "WebAssembly import returned the wrong number of values"); ok = false;
      }
      for (size_t i = 0; ok && i < type.results.size(); ++i) {
        ok = from_js(ctx, s, type.results[i], values.values[i], &results[i]);
      }
    }
    JS_FreeValue(ctx, result);
    if (!ok) { *trap = wi::Trap::New(s->store, "JavaScript import threw"); return Result::Error; }
    return Result::Ok;
  });
  func->set_host_info(binding);
  func->set_finalizer([binding](wi::Object *) { delete binding; });
  JSValue anchor = wrapper(s->active, s, func.ref(), FunctionKind);
  s->active_roots->push_back(anchor);
  return func;
}
static JSValue instance_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                              JSValueConst *argv, int, JSValue *data) {
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Instance requires new");
  Handle *mh = handle(ctx, arg(argc, argv, 0), ModuleKind);
  if (!mh) return JS_EXCEPTION;
  State *s = get_state(data[0]); Active active(s, ctx);
  if (mh->state != s) return wasm_error(ctx, s, LinkError, "Module belongs to another WebAssembly store");
  auto mod = mh->object.As<wi::Module>();
  JSValue imports = arg(argc, argv, 1);
  if (!JS_IsUndefined(imports) && !JS_IsObject(imports))
    return JS_ThrowTypeError(ctx, "WebAssembly imports must be an object");
  wi::RefVec refs;
  std::vector<wi::Object::Ptr> roots;
  for (const auto &item : mod->import_types()) {
    if (!JS_IsObject(imports)) return JS_ThrowTypeError(ctx, "WebAssembly module requires imports");
    JSValue module = named_property(ctx, imports, item.module);
    if (JS_IsException(module)) return module;
    if (!JS_IsObject(module)) {
      JS_FreeValue(ctx, module); return JS_ThrowTypeError(ctx, "WebAssembly import module must be an object");
    }
    JSValue value = named_property(ctx, module, item.name);
    JS_FreeValue(ctx, module);
    if (JS_IsException(value)) return value;
    wi::Object::Ptr object;
    if (item.type->kind == wi::ExternKind::Func) {
      wi::Ref ref;
      auto marker = static_cast<SuspendingData *>(JS_GetOpaque(value, classes[SuspendingKind]));
      if (marker) object = host_func(s, marker->function, *static_cast<const wi::FuncType *>(item.type.get()), true);
      else if (function_ref(s, value, &ref)) object = wi::Object::Ptr(s->store, ref);
      else if (JS_IsFunction(ctx, value)) object = host_func(s, value,
          *static_cast<const wi::FuncType *>(item.type.get()));
    } else {
      Kind kind = item.type->kind == wi::ExternKind::Memory ? MemoryKind :
                  item.type->kind == wi::ExternKind::Table ? TableKind : GlobalKind;
      auto h = static_cast<Handle *>(JS_GetOpaque(value, classes[kind]));
      if (h && h->state == s) object = h->object;
      else if (item.type->kind == wi::ExternKind::Global) {
        const auto &type = *static_cast<const wi::GlobalType *>(item.type.get());
        wi::Value initial;
        bool valid_primitive = type.type == Type::I64 ? JS_IsBigInt(ctx, value) :
            (type.type == Type::ExternRef || type.type == Type::FuncRef || JS_IsNumber(value));
        if (type.mut == wi::Mutability::Const && valid_primitive &&
            from_js(ctx, s, type.type, value, &initial))
          object = wi::Global::New(s->store, type, initial);
      }
    }
    JS_FreeValue(ctx, value);
    if (!object) {
      if (JS_HasException(ctx)) return JS_EXCEPTION;
      return wasm_error(ctx, s, LinkError, "WebAssembly import has an incompatible type");
    }
    wi::Trap::Ptr trap;
    auto ext = object.As<wi::Extern>();
    if (wabt::Failed(ext->Match(s->store, item, &trap)))
      return wasm_error(ctx, s, LinkError, trap->message().c_str());
    refs.push_back(object.ref()); roots.push_back(std::move(object));
  }
  wi::Trap::Ptr trap;
  // Resource construction and active segments have one owner and one pass.
  // Delay start until the native dispatch has been installed; fallback follows
  // exactly the same start-once path rather than retrying after execution.
  auto instance = wi::Instance::Instantiate(s->store, mod.ref(), refs, &trap, false);
  if (instance) {
    auto native = static_cast<std::shared_ptr<NativeModule> *>(mod->host_info());
    if (native && *native) bind_native(s, instance, *native);
    for (const auto& start : mod->desc().starts) {
      auto function = s->store.UnsafeGet<wi::Func>(instance->funcs()[start.func_index]);
      wi::Values returns;
      if (wabt::Failed(function->Call(s->store, {}, returns, &trap))) { instance.reset(); break; }
    }
  }
  if (!instance) {
    ++s->stats.traps;
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    return wasm_error(ctx, s, RuntimeError, trap ? trap->message().c_str() : "Instantiation failed");
  }
  ++s->stats.instances;
  JSValue value = wrapper(ctx, s, instance.ref(), InstanceKind, new_target);
  if (JS_IsException(value)) return value;
  JSValue exports = JS_NewObjectProto(ctx, JS_NULL);
  for (size_t i = 0; i < instance->exports().size(); ++i) {
    const auto &type = mod->export_types()[i];
    Kind kind;
    switch (type.type->kind) {
      case wi::ExternKind::Func: kind = FunctionKind; break;
      case wi::ExternKind::Memory: kind = MemoryKind; break;
      case wi::ExternKind::Table: kind = TableKind; break;
      case wi::ExternKind::Global: kind = GlobalKind; break;
      default:
        JS_FreeValue(ctx, exports); JS_FreeValue(ctx, value);
        return wasm_error(ctx, s, LinkError, "WebAssembly tag exports are not yet exposed");
    }
    JSValue item = wrapper(ctx, s, instance->exports()[i], kind);
    JSAtom name = JS_NewAtomLen(ctx, type.name.data(), type.name.size());
    int defined = JS_IsException(item) ? -1 : JS_DefinePropertyValue(ctx, exports,
           name, item, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, name);
    if (defined < 0) {
      JS_FreeValue(ctx, exports); JS_FreeValue(ctx, value); return JS_EXCEPTION;
    }
  }
  JS_PreventExtensions(ctx, exports);
  handle(ctx, value, InstanceKind)->exports = exports;
  return value;
}
static JSValue instance_exports(JSContext *ctx, JSValueConst this_val) {
  Handle *h = handle(ctx, this_val, InstanceKind);
  return h ? JS_DupValue(ctx, h->exports) : JS_EXCEPTION;
}
static bool address_index(JSContext *ctx, JSValueConst value, bool is64, uint64_t *out) {
  if (is64) {
    uint64_t high;
    int r = JS_ToBigInt128Sat(ctx, out, &high, value);
    if (r < 0) return false;
    if (r || high) {
      JS_ThrowRangeError(ctx, "WebAssembly address is outside the unsigned 64-bit range");
      return false;
    }
    return true;
  }
  if (JS_ToIndex(ctx, out, value)) return false;
  if (*out > UINT32_MAX) {
    JS_ThrowRangeError(ctx, "WebAssembly address is outside the unsigned 32-bit range");
    return false;
  }
  return true;
}
static bool limits(JSContext *ctx, JSValueConst desc, bool memory,
                    wabt::Limits *out) {
  if (!JS_IsObject(desc)) { JS_ThrowTypeError(ctx, "Expected WebAssembly descriptor"); return false; }
  JSValue address = JS_GetPropertyStr(ctx, desc, "address");
  if (!JS_IsUndefined(address)) {
    const char *str = JS_ToCString(ctx, address);
    if (!str) { JS_FreeValue(ctx, address); return false; }
    bool valid = !strcmp(str, "i32") || !strcmp(str, "i64");
    out->is_64 = !strcmp(str, "i64"); JS_FreeCString(ctx, str);
    if (!valid) { JS_FreeValue(ctx, address); JS_ThrowTypeError(ctx, "Invalid WebAssembly address type"); return false; }
  }
  JS_FreeValue(ctx, address);
  JSValue initial = JS_GetPropertyStr(ctx, desc, "initial");
  if (JS_IsUndefined(initial)) {
    JS_FreeValue(ctx, initial);
    initial = JS_GetPropertyStr(ctx, desc, "minimum");
  }
  if (JS_IsUndefined(initial)) {
    JS_ThrowTypeError(ctx, "WebAssembly descriptor requires initial or minimum"); return false;
  }
  bool ok = address_index(ctx, initial, out->is_64, &out->initial); JS_FreeValue(ctx, initial);
  if (!ok) return false;
  JSValue maximum = JS_GetPropertyStr(ctx, desc, "maximum");
  out->has_max = !JS_IsUndefined(maximum);
  if (out->has_max) ok = address_index(ctx, maximum, out->is_64, &out->max);
  JS_FreeValue(ctx, maximum);
  if (!ok) return false;
  JSValue shared = JS_GetPropertyStr(ctx, desc, "shared");
  out->is_shared = JS_ToBool(ctx, shared); JS_FreeValue(ctx, shared);
  if (out->is_shared) { JS_ThrowTypeError(ctx, "Shared WebAssembly memory requires a concurrent interpreter"); return false; }
  // This is the core memory32 address-space bound, not a product quota.
  uint64_t bound = memory ? (uint64_t(1) << (out->is_64 ? 48 : 16)) : UINT32_MAX;
  if (out->initial > bound || (out->has_max && (out->max < out->initial || out->max > bound))) {
    JS_ThrowRangeError(ctx, "WebAssembly limits exceed the address space"); return false;
  }
  if (memory && out->initial > SIZE_MAX / WABT_DEFAULT_PAGE_SIZE) {
    JS_ThrowRangeError(ctx, "WebAssembly memory size overflows host storage"); return false;
  }
  return true;
}
static void free_alias(JSRuntime *, void *, void *) {}
static JSValue memory_buffer(JSContext *ctx, JSValueConst this_val) {
  Handle *h = handle(ctx, this_val, MemoryKind); if (!h) return JS_EXCEPTION;
  if (!JS_IsUndefined(h->buffer)) return JS_DupValue(ctx, h->buffer);
  auto mem = h->object.As<wi::Memory>();
  h->buffer = JS_NewArrayBuffer(ctx, mem->UnsafeData(), mem->ByteSize(), free_alias, nullptr, false);
  if (JS_IsException(h->buffer)) { h->buffer = JS_UNDEFINED; return JS_EXCEPTION; }
  if (JS_SetArrayBufferTransferable(ctx, h->buffer, false) < 0) {
    JS_FreeValue(ctx, h->buffer); h->buffer = JS_UNDEFINED; return JS_EXCEPTION;
  }
  // A buffer can outlive its Memory wrapper. An immutable private Symbol edge
  // keeps the native allocation alive and is visible to QuickJS cycle marking.
  JS_DefinePropertyValue(ctx, h->buffer, h->state->buffer_owner,
                        JS_DupValue(ctx, this_val), 0);
  return JS_DupValue(ctx, h->buffer);
}
static void observe_memory(State *s, wi::Memory::Ptr memory) {
  size_t ref = memory.ref().index;
  memory->set_grow_callback([s, ref]() {
    auto i = s->wrappers.find(ref);
    if (i == s->wrappers.end()) return;
    auto h = static_cast<Handle *>(JS_GetOpaque(i->second, classes[MemoryKind]));
    if (h && !JS_IsUndefined(h->buffer)) {
      JS_DetachArrayBuffer(s->active, h->buffer);
      JS_FreeValue(s->active, h->buffer); h->buffer = JS_UNDEFINED;
    }
  });
}
static JSValue memory_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv, int, JSValue *data) {
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Memory requires new");
  State *s = get_state(data[0]); Active active(s, ctx);
  wabt::Limits l;
  if (!limits(ctx, arg(argc, argv, 0), true, &l)) return JS_EXCEPTION;
  auto mem = wi::Memory::New(s->store, wi::MemoryType(l, WABT_DEFAULT_PAGE_SIZE));
  if (!mem) { s->collect(); return JS_ThrowRangeError(ctx, "WebAssembly memory allocation failed"); }
  observe_memory(s, mem);
  return wrapper(ctx, s, mem.ref(), MemoryKind, new_target);
}
static JSValue memory_grow(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  Handle *h = handle(ctx, this_val, MemoryKind); if (!h) return JS_EXCEPTION;
  State *s = h->state; Active active(s, ctx);
  uint64_t count;
  auto mem = h->object.As<wi::Memory>();
  if (!address_index(ctx, arg(argc, argv, 0), mem->type().limits.is_64, &count)) return JS_EXCEPTION;
  // WABT's PageSize() accessor returns the current page count (pages_),
  // whereas type().page_size is the byte size of one WebAssembly page.
  uint64_t old = mem->PageSize();
  if (wabt::Failed(mem->Grow(count))) return JS_ThrowRangeError(ctx, "WebAssembly memory cannot grow");
  return mem->type().limits.is_64 ? JS_NewBigUint64(ctx, old) : JS_NewUint32(ctx, old);
}
static bool value_type(JSContext *ctx, JSValueConst v, Type *out) {
  const char *str = JS_ToCString(ctx, v); if (!str) return false;
  bool ok = true;
  if (!strcmp(str, "i32")) *out = Type::I32;
  else if (!strcmp(str, "i64")) *out = Type::I64;
  else if (!strcmp(str, "f32")) *out = Type::F32;
  else if (!strcmp(str, "f64")) *out = Type::F64;
  else if (!strcmp(str, "externref")) *out = Type::ExternRef;
  else if (!strcmp(str, "anyfunc") || !strcmp(str, "funcref")) *out = Type::FuncRef;
  else { JS_ThrowTypeError(ctx, "Unsupported WebAssembly value type"); ok = false; }
  JS_FreeCString(ctx, str); return ok;
}
static JSValue global_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv, int, JSValue *data) {
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Global requires new");
  State *s = get_state(data[0]); Active active(s, ctx);
  JSValue desc = arg(argc, argv, 0);
  if (!JS_IsObject(desc)) return JS_ThrowTypeError(ctx, "Expected WebAssembly global descriptor");
  JSValue name = JS_GetPropertyStr(ctx, desc, "value"); Type type;
  bool ok = value_type(ctx, name, &type); JS_FreeValue(ctx, name); if (!ok) return JS_EXCEPTION;
  JSValue mut = JS_GetPropertyStr(ctx, desc, "mutable");
  int mutable_ = JS_ToBool(ctx, mut); JS_FreeValue(ctx, mut);
  wi::Value initial;
  JSValue value = arg(argc, argv, 1);
  if (argc < 2 && type != Type::ExternRef) {
    if (type == Type::FuncRef) initial = wi::Value::Make(wi::Ref::Null);
    else if (type == Type::I64) initial = wi::Value::Make(int64_t(0));
    else if (type == Type::F32) initial = wi::Value::Make(float(0));
    else if (type == Type::F64) initial = wi::Value::Make(double(0));
    else initial = wi::Value::Make(int32_t(0));
  } else if (!from_js(ctx, s, type, value, &initial)) return JS_EXCEPTION;
  auto global = wi::Global::New(s->store, wi::GlobalType(type,
      mutable_ ? wi::Mutability::Var : wi::Mutability::Const), initial);
  return wrapper(ctx, s, global.ref(), GlobalKind, new_target);
}
static JSValue global_get(JSContext *ctx, JSValueConst this_val) {
  Handle *h = handle(ctx, this_val, GlobalKind); if (!h) return JS_EXCEPTION;
  Active active(h->state, ctx);
  auto global = h->object.As<wi::Global>();
  return to_js(ctx, h->state, global->type().type, global->Get());
}
static JSValue global_set(JSContext *ctx, JSValueConst this_val, JSValueConst value) {
  Handle *h = handle(ctx, this_val, GlobalKind); if (!h) return JS_EXCEPTION;
  Active active(h->state, ctx);
  auto global = h->object.As<wi::Global>();
  if (global->type().mut != wi::Mutability::Var)
    return JS_ThrowTypeError(ctx, "WebAssembly global is immutable");
  wi::Value v;
  if (!from_js(ctx, h->state, global->type().type, value, &v)) return JS_EXCEPTION;
  global->UnsafeSet(v); return JS_UNDEFINED;
}
static JSValue global_value_of(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) {
  return global_get(ctx, this_val);
}
static JSValue table_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv, int, JSValue *data) {
  if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Table requires new");
  State *s = get_state(data[0]); Active active(s, ctx);
  JSValue desc = arg(argc, argv, 0); wabt::Limits l;
  if (!limits(ctx, desc, false, &l)) return JS_EXCEPTION;
  JSValue element = JS_GetPropertyStr(ctx, desc, "element"); Type type;
  bool ok = value_type(ctx, element, &type); JS_FreeValue(ctx, element);
  if (!ok) return JS_EXCEPTION;
  if (type != Type::FuncRef && type != Type::ExternRef)
    return JS_ThrowTypeError(ctx, "WebAssembly table requires a reference type");
  wi::Value value;
  JSValue initial = arg(argc, argv, 1);
  if (argc < 2) initial = type == Type::FuncRef ? JS_NULL : JS_UNDEFINED;
  if (!from_js(ctx, s, type, initial, &value)) return JS_EXCEPTION;
  auto table = wi::Table::New(s->store, wi::TableType(type, l), value.Get<wi::Ref>());
  if (!table) { s->collect(); return JS_ThrowRangeError(ctx, "WebAssembly table allocation failed"); }
  return wrapper(ctx, s, table.ref(), TableKind, new_target);
}
static JSValue table_length(JSContext *ctx, JSValueConst this_val) {
  Handle *h = handle(ctx, this_val, TableKind); if (!h) return JS_EXCEPTION;
  auto table = h->object.As<wi::Table>();
  return table->type().limits.is_64 ? JS_NewBigUint64(ctx, table->size()) : JS_NewUint32(ctx, table->size());
}
static JSValue table_op(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int op) {
  Handle *h = handle(ctx, this_val, TableKind); if (!h) return JS_EXCEPTION;
  Active active(h->state, ctx);
  auto table = h->object.As<wi::Table>(); uint64_t index;
  if (!address_index(ctx, arg(argc, argv, 0), table->type().limits.is_64, &index)) return JS_EXCEPTION;
  if (op == 0) {
    wi::Ref ref;
    if (wabt::Failed(table->Get(index, &ref))) return JS_ThrowRangeError(ctx, "WebAssembly table index out of bounds");
    return to_js(ctx, h->state, table->type().element, wi::Value::Make(ref));
  }
  wi::Value v;
  JSValue value = arg(argc, argv, 1);
  if ((op == 1 || op == 2) && argc < 2 && table->type().element == Type::FuncRef) value = JS_NULL;
  if (!from_js(ctx, h->state, table->type().element, value, &v)) return JS_EXCEPTION;
  uint32_t old = table->size();
  Result r = op == 1 ? table->Set(h->state->store, index, v.Get<wi::Ref>()) :
                      table->Grow(h->state->store, index, v.Get<wi::Ref>());
  if (wabt::Failed(r)) return JS_ThrowRangeError(ctx, "WebAssembly table operation out of bounds");
  return op == 1 ? JS_UNDEFINED : table->type().limits.is_64 ? JS_NewBigUint64(ctx, old) : JS_NewUint32(ctx, old);
}
static JSValue table_get(JSContext *c, JSValueConst t, int n, JSValueConst *a) { return table_op(c,t,n,a,0); }
static JSValue table_set(JSContext *c, JSValueConst t, int n, JSValueConst *a) { return table_op(c,t,n,a,1); }
static JSValue table_grow(JSContext *c, JSValueConst t, int n, JSValueConst *a) { return table_op(c,t,n,a,2); }
static Kind reference_kind(State *s, wi::Ref ref) {
  if (s->store.Is<wi::Instance>(ref)) return InstanceKind;
  if (s->store.Is<wi::Module>(ref)) return ModuleKind;
  if (s->store.Is<wi::Memory>(ref)) return MemoryKind;
  if (s->store.Is<wi::Table>(ref)) return TableKind;
  if (s->store.Is<wi::Global>(ref)) return GlobalKind;
  if (s->store.Is<wi::Foreign>(ref)) return ForeignKind;
  return FunctionKind;
}
static JSValue anchor(JSContext *ctx, State *s, wi::Ref ref) {
  if (ref == wi::Ref::Null) return JS_UNDEFINED;
  return wrapper(ctx, s, ref, reference_kind(s, ref));
}
static void table_references(State *s, size_t ref, uint64_t start, uint64_t count) {
  auto i = s->wrappers.find(ref); if (i == s->wrappers.end()) return;
  auto h = static_cast<Handle *>(JS_GetOpaque(i->second, classes[TableKind]));
  if (!h) return;
  auto table = h->object.As<wi::Table>();
  h->references.resize(table->size(), JS_UNDEFINED);
  for (uint64_t j = start; j < start + count; ++j) {
    JSValue value = anchor(s->active, s, table->UnsafeGet(j));
    JSValue old = h->references[j]; h->references[j] = value;
    JS_FreeValueRT(s->rt, old);
  }
}
static void global_reference(State *s, size_t ref) {
  auto i = s->wrappers.find(ref); if (i == s->wrappers.end()) return;
  auto h = static_cast<Handle *>(JS_GetOpaque(i->second, classes[GlobalKind]));
  if (!h) return;
  auto global = h->object.As<wi::Global>();
  if (!wi::IsReference(global->type().type)) return;
  JSValue value = anchor(s->active, s, global->Get().Get<wi::Ref>());
  JSValue old = h->references.empty() ? JS_UNDEFINED : h->references[0];
  h->references.resize(1, JS_UNDEFINED); h->references[0] = value;
  JS_FreeValueRT(s->rt, old);
}
static void connect_references(JSContext *ctx, Handle *h, Kind kind) {
  State *s = h->state;
  if (kind == ForeignKind) h->binding = static_cast<Binding *>(h->object.As<wi::Foreign>()->ptr());
  else if (kind == FunctionKind) {
    if (h->object->kind() == wi::ObjectKind::HostFunc)
      h->binding = static_cast<Binding *>(h->object->host_info());
    else h->references.push_back(anchor(ctx, s, h->object.As<wi::DefinedFunc>()->instance()));
  } else if (kind == InstanceKind) {
    auto instance = h->object.As<wi::Instance>();
    h->references.push_back(anchor(ctx, s, instance->module()));
    for (auto ref : instance->imports()) h->references.push_back(anchor(ctx, s, ref));
    for (auto ref : instance->tables()) h->references.push_back(anchor(ctx, s, ref));
    for (auto ref : instance->globals()) h->references.push_back(anchor(ctx, s, ref));
    for (auto ref : instance->memories()) h->references.push_back(anchor(ctx, s, ref));
  } else if (kind == TableKind) {
    auto table = h->object.As<wi::Table>(); size_t ref = table.ref().index;
    table->set_change_callback([s, ref](uint64_t start, uint64_t count) {
      table_references(s, ref, start, count);
    });
    table_references(s, ref, 0, table->size());
  } else if (kind == GlobalKind) {
    auto global = h->object.As<wi::Global>(); size_t ref = global.ref().index;
    global->set_change_callback([s, ref]() { global_reference(s, ref); });
    global_reference(s, ref);
  }
}
static void method(JSContext *ctx, JSValueConst proto, const char *name,
                    JSCFunction *fn, int length) {
  JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                          JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}
static void accessor(JSContext *ctx, JSValueConst proto, const char *name,
                      JSValue (*get)(JSContext *, JSValueConst),
                      JSValue (*set)(JSContext *, JSValueConst, JSValueConst) = nullptr) {
  JSCFunctionType g = {}, v = {}; g.getter = get; v.setter = set;
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(ctx, proto, atom,
      JS_NewCFunction2(ctx, g.generic, name, 0, JS_CFUNC_getter, 0),
      set ? JS_NewCFunction2(ctx, v.generic, name, 1, JS_CFUNC_setter, 0) : JS_UNDEFINED,
      JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, atom);
}
static JSValue constructor_call(JSContext *ctx, JSValueConst function,
                                 JSValueConst this_val, int argc,
                                 JSValueConst *argv, int flags) {
  auto c = static_cast<ConstructorData *>(JS_GetOpaque(function, classes[ConstructorKind]));
  if (c->require_new && !(flags & JS_CALL_FLAG_CONSTRUCTOR))
    return JS_ThrowTypeError(ctx, "WebAssembly constructor requires new");
  return c->function(ctx, flags & JS_CALL_FLAG_CONSTRUCTOR ? this_val : JS_UNDEFINED,
                     argc, argv, c->magic, &c->owner);
}
static JSValue constructor(JSContext *ctx, State *s, const char *name,
                            JSCFunctionData *fn, int length, int magic,
                            bool require_new) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue function = JS_GetPropertyStr(ctx, global, "Function");
  JSValue proto = JS_GetPropertyStr(ctx, function, "prototype");
  JSValue result = JS_NewObjectProtoClass(ctx, proto, classes[ConstructorKind]);
  JS_FreeValue(ctx, proto); JS_FreeValue(ctx, function); JS_FreeValue(ctx, global);
  if (JS_IsException(result)) return result;
  auto c = new ConstructorData{s->shared_from_this(), JS_DupValue(ctx, s->owner), fn, magic, require_new};
  JS_SetOpaque(result, c); JS_SetConstructorBit(ctx, result, true);
  JS_DefinePropertyValueStr(ctx, result, "name", JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(ctx, result, "length", JS_NewInt32(ctx, length), JS_PROP_CONFIGURABLE);
  return result;
}
} // namespace

extern "C" int web_js_wasm_init(JSContext *ctx, int (*poll)(JSContext *),
                                int (*can_compile)(JSContext *)) {
  const char *names[] = {"WebAssemblyStore", "WebAssembly.Module", "WebAssembly.Instance",
      "WebAssembly.Memory", "WebAssembly.Table", "WebAssembly.Global", "WebAssembly.Function",
      "WebAssembly.ExternRef", "WebAssembly.Constructor", "WebAssembly.Suspending", "WebAssembly.Continuation"};
  for (int i = 0; i < KindCount; ++i) {
    if (!classes[i]) JS_NewClassID(&classes[i]);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), classes[i])) {
      JSClassDef def = {}; def.class_name = names[i]; def.finalizer = finalizer; def.gc_mark = mark;
      if (i == ConstructorKind) def.call = constructor_call;
      if (JS_NewClass(JS_GetRuntime(ctx), classes[i], &def)) return -1;
    }
  }
  auto lifetime = std::make_shared<State>(ctx, poll, can_compile);
  auto s = lifetime.get();
  JSValue owner = JS_NewObjectClass(ctx, classes[StateKind]);
  if (JS_IsException(owner)) return -1;
  s->owner = owner; JS_SetOpaque(owner, new std::shared_ptr<State>(lifetime));
  JSValue symbol = JS_NewSymbol(ctx, "WebAssembly memory owner", false);
  s->buffer_owner = JS_ValueToAtom(ctx, symbol); JS_FreeValue(ctx, symbol);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
  JSValue iterator_symbol = JS_GetPropertyStr(ctx, symbol_ctor, "iterator");
  s->iterator = JS_ValueToAtom(ctx, iterator_symbol);
  JS_FreeValue(ctx, iterator_symbol); JS_FreeValue(ctx, symbol_ctor);
  JSValue data_view = JS_GetPropertyStr(ctx, global, "DataView");
  JSValue data_view_proto = JS_GetPropertyStr(ctx, data_view, "prototype");
  const char *view_properties[] = {"buffer", "byteOffset", "byteLength"};
  for (int i = 0; i < 3; ++i) {
    JSAtom atom = JS_NewAtom(ctx, view_properties[i]); JSPropertyDescriptor descriptor = {};
    if (JS_GetOwnProperty(ctx, &descriptor, data_view_proto, atom) > 0) {
      s->data_view_getters[i] = descriptor.getter;
      JS_FreeValue(ctx, descriptor.value); JS_FreeValue(ctx, descriptor.setter);
    }
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, data_view_proto); JS_FreeValue(ctx, data_view);
  JSValue space = JS_NewObject(ctx);
  const char *errors[] = {"CompileError", "LinkError", "RuntimeError", "SuspendError"};
  JSValue error_global = JS_GetPropertyStr(ctx, global, "Error");
  JSValue error_proto = JS_GetPropertyStr(ctx, error_global, "prototype");
  for (int i = 0; i < ErrorCount; ++i) {
    s->errors[i] = JS_NewObjectProto(ctx, error_proto);
    JS_DefinePropertyValueStr(ctx, s->errors[i], "name", JS_NewString(ctx, errors[i]),
                            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JSValue ctor = constructor(ctx, s, errors[i], error_ctor, 1, i, false);
    JS_SetConstructorBit(ctx, ctor, true); JS_SetConstructor(ctx, ctor, s->errors[i]);
    JS_SetPropertyStr(ctx, space, errors[i], ctor);
  }
  JS_FreeValue(ctx, error_proto); JS_FreeValue(ctx, error_global);
  struct Constructor { Kind kind; const char *name; JSCFunctionData *fn; int length; };
  const Constructor constructors[] = {
    {ModuleKind,"Module",module_ctor,1}, {InstanceKind,"Instance",instance_ctor,1},
    {MemoryKind,"Memory",memory_ctor,1}, {TableKind,"Table",table_ctor,1},
    {GlobalKind,"Global",global_ctor,1}, {SuspendingKind,"Suspending",suspending_ctor,1}
  };
  for (const auto &c : constructors) {
    JSValue proto = JS_NewObject(ctx);
    JSValue ctor = constructor(ctx, s, c.name, c.fn, c.length, 0, true);
    JS_SetConstructorBit(ctx, ctor, true); JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, classes[c.kind], JS_DupValue(ctx, proto));
    if (c.kind == ModuleKind) {
      JS_SetPropertyStr(ctx, ctor, "imports", JS_NewCFunctionData(ctx, module_types, 1, 1, 0, nullptr));
      JS_SetPropertyStr(ctx, ctor, "exports", JS_NewCFunctionData(ctx, module_types, 1, 0, 0, nullptr));
      JS_SetPropertyStr(ctx, ctor, "customSections", JS_NewCFunctionData(ctx, custom_sections, 2, 0, 0, nullptr));
    } else if (c.kind == InstanceKind) {
      accessor(ctx, proto, "exports", instance_exports);
    } else if (c.kind == MemoryKind) {
      accessor(ctx, proto, "buffer", memory_buffer); method(ctx, proto, "grow", memory_grow, 1);
    } else if (c.kind == TableKind) {
      accessor(ctx, proto, "length", table_length);
      method(ctx, proto, "get", table_get, 1); method(ctx, proto, "set", table_set, 2);
      method(ctx, proto, "grow", table_grow, 1);
    } else if (c.kind == GlobalKind) {
      accessor(ctx, proto, "value", global_get, global_set); method(ctx, proto, "valueOf", global_value_of, 0);
    }
    JS_SetPropertyStr(ctx, space, c.name, ctor); JS_FreeValue(ctx, proto);
  }
  JS_SetPropertyStr(ctx, space, "validate", JS_NewCFunctionData(ctx, validate, 1, 0, 1, &owner));
  JSValue promise_wrapper = JS_NewCFunctionData(ctx, promising, 1, 0, 1, &owner);
  JS_DefinePropertyValueStr(ctx, promise_wrapper, "name", JS_NewString(ctx, "promising"), JS_PROP_CONFIGURABLE);
  JS_SetPropertyStr(ctx, space, "promising", promise_wrapper);
  JS_SetPropertyStr(ctx, global, "WebAssembly", space);
  states[ctx] = lifetime;
  JS_FreeValue(ctx, owner); JS_FreeValue(ctx, global);
  return JS_HasException(ctx) ? -1 : 0;
}

extern "C" int web_js_wasm_get_stats(JSContext *ctx, web_js_wasm_stats *out) {
  if (!out) return -1;
  auto entry = states.find(ctx); if (entry == states.end()) return -1;
  auto state = entry->second.lock(); if (!state) return -1;
  *out = state->stats; out->live_wrappers = state->wrappers.size();
  return 0;
}

extern "C" void web_js_wasm_collect(JSContext *ctx) {
  auto entry = states.find(ctx); if (entry == states.end()) return;
  auto state = entry->second.lock(); if (!state || state->active) return;
  state->flush_collection();
}

extern "C" void web_js_wasm_cancel(JSContext *ctx) {
  auto entry = states.find(ctx); if (entry == states.end()) return;
  auto state = entry->second.lock(); if (!state) return;
  state->retired = true;
  std::vector<JSValue> tasks;
  for (auto *task : state->tasks) tasks.push_back(JS_DupValueRT(state->rt, task->self));
  for (auto root : tasks) {
    auto *task = static_cast<PromiseTask *>(JS_GetOpaque(root, classes[PromiseTaskKind]));
    task->cancel();
    if (task->done && !task->running && task->state) task->cleanup();
    JS_FreeValueRT(state->rt, root);
  }
}

extern "C" const char *web_js_wasm_native_fallback(JSContext *ctx) {
  auto entry = states.find(ctx); if (entry == states.end()) return "";
  auto state = entry->second.lock(); if (!state) return "";
  return state->native_fallback.c_str();
}
