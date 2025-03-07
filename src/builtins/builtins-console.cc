// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api/api-inl.h"
#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/debug/interface-types.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"
#include "src/objects/objects-inl.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// Console

#define CONSOLE_METHOD_LIST(V)      \
  V(Debug, debug)                   \
  V(Error, error)                   \
  V(Info, info)                     \
  V(Log, log)                       \
  V(Warn, warn)                     \
  V(Dir, dir)                       \
  V(DirXml, dirXml)                 \
  V(Table, table)                   \
  V(Trace, trace)                   \
  V(Group, group)                   \
  V(GroupCollapsed, groupCollapsed) \
  V(GroupEnd, groupEnd)             \
  V(Clear, clear)                   \
  V(Count, count)                   \
  V(CountReset, countReset)         \
  V(Assert, assert)                 \
  V(Profile, profile)               \
  V(ProfileEnd, profileEnd)         \
  V(TimeLog, timeLog)

namespace {
void ConsoleCall(
    Isolate* isolate, const internal::BuiltinArguments& args,
    void (debug::ConsoleDelegate::*func)(const v8::debug::ConsoleCallArguments&,
                                         const v8::debug::ConsoleContext&)) {
  CHECK(!isolate->has_pending_exception());
  CHECK(!isolate->has_scheduled_exception());
  if (!isolate->console_delegate()) return;
  HandleScope scope(isolate);

  // Access check. The current context has to match the context of all
  // arguments, otherwise the inspector might leak objects across contexts.
  Handle<Context> context = handle(isolate->context(), isolate);
  for (int i = 0; i < args.length(); ++i) {
    Handle<Object> argument = args.at<Object>(i);
    if (!argument->IsJSObject()) continue;

    Handle<JSObject> argument_obj = Handle<JSObject>::cast(argument);
    if (argument->IsAccessCheckNeeded(isolate) &&
        !isolate->MayAccess(context, argument_obj)) {
      isolate->ReportFailedAccessCheck(argument_obj);
      return;
    }
  }

  debug::ConsoleCallArguments wrapper(args);
  Handle<Object> context_id_obj = JSObject::GetDataProperty(
      args.target(), isolate->factory()->console_context_id_symbol());
  int context_id =
      context_id_obj->IsSmi() ? Handle<Smi>::cast(context_id_obj)->value() : 0;
  Handle<Object> context_name_obj = JSObject::GetDataProperty(
      args.target(), isolate->factory()->console_context_name_symbol());
  Handle<String> context_name = context_name_obj->IsString()
                                    ? Handle<String>::cast(context_name_obj)
                                    : isolate->factory()->anonymous_string();
  (isolate->console_delegate()->*func)(
      wrapper,
      v8::debug::ConsoleContext(context_id, Utils::ToLocal(context_name)));
}

void LogTimerEvent(Isolate* isolate, BuiltinArguments args,
                   Logger::StartEnd se) {
  if (!isolate->logger()->is_logging()) return;
  HandleScope scope(isolate);
  std::unique_ptr<char[]> name;
  const char* raw_name = "default";
  if (args.length() > 1 && args[1].IsString()) {
    // Try converting the first argument to a string.
    name = args.at<String>(1)->ToCString();
    raw_name = name.get();
  }
  LOG(isolate, TimerEvent(se, raw_name));
}
}  // namespace

#define CONSOLE_BUILTIN_IMPLEMENTATION(call, name)             \
  BUILTIN(Console##call) {                                     \
    ConsoleCall(isolate, args, &debug::ConsoleDelegate::call); \
    RETURN_FAILURE_IF_SCHEDULED_EXCEPTION(isolate);            \
    return ReadOnlyRoots(isolate).undefined_value();           \
  }
CONSOLE_METHOD_LIST(CONSOLE_BUILTIN_IMPLEMENTATION)
#undef CONSOLE_BUILTIN_IMPLEMENTATION

BUILTIN(ConsoleTime) {
  LogTimerEvent(isolate, args, Logger::START);
  ConsoleCall(isolate, args, &debug::ConsoleDelegate::Time);
  RETURN_FAILURE_IF_SCHEDULED_EXCEPTION(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

BUILTIN(ConsoleTimeEnd) {
  LogTimerEvent(isolate, args, Logger::END);
  ConsoleCall(isolate, args, &debug::ConsoleDelegate::TimeEnd);
  RETURN_FAILURE_IF_SCHEDULED_EXCEPTION(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

BUILTIN(ConsoleTimeStamp) {
  LogTimerEvent(isolate, args, Logger::STAMP);
  ConsoleCall(isolate, args, &debug::ConsoleDelegate::TimeStamp);
  RETURN_FAILURE_IF_SCHEDULED_EXCEPTION(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {

void InstallContextFunction(Isolate* isolate, Handle<JSObject> target,
                            const char* name, Builtin builtin_id,
                            int context_id, Handle<Object> context_name) {
  Factory* const factory = isolate->factory();

  Handle<NativeContext> context(isolate->native_context());
  Handle<Map> map = isolate->sloppy_function_without_prototype_map();

  Handle<String> name_string =
      Name::ToFunctionName(isolate, factory->InternalizeUtf8String(name))
          .ToHandleChecked();
  Handle<SharedFunctionInfo> info =
      factory->NewSharedFunctionInfoForBuiltin(name_string, builtin_id);
  info->set_language_mode(LanguageMode::kSloppy);

  Handle<JSFunction> fun =
      Factory::JSFunctionBuilder{isolate, info, context}.set_map(map).Build();

  fun->shared().set_native(true);
  fun->shared().DontAdaptArguments();
  fun->shared().set_length(1);

  JSObject::AddProperty(isolate, fun, factory->console_context_id_symbol(),
                        handle(Smi::FromInt(context_id), isolate), NONE);
  if (context_name->IsString()) {
    JSObject::AddProperty(isolate, fun, factory->console_context_name_symbol(),
                          context_name, NONE);
  }
  JSObject::AddProperty(isolate, target, name_string, fun, NONE);
}

}  // namespace

BUILTIN(ConsoleContext) {
  HandleScope scope(isolate);

  Factory* const factory = isolate->factory();
  Handle<String> name = factory->InternalizeUtf8String("Context");
  Handle<SharedFunctionInfo> info =
      factory->NewSharedFunctionInfoForBuiltin(name, Builtin::kIllegal);
  info->set_language_mode(LanguageMode::kSloppy);

  Handle<JSFunction> cons =
      Factory::JSFunctionBuilder{isolate, info, isolate->native_context()}
          .Build();

  Handle<JSObject> prototype = factory->NewJSObject(isolate->object_function());
  JSFunction::SetPrototype(cons, prototype);

  Handle<JSObject> context = factory->NewJSObject(cons, AllocationType::kOld);
  DCHECK(context->IsJSObject());
  int id = isolate->last_console_context_id() + 1;
  isolate->set_last_console_context_id(id);

#define CONSOLE_BUILTIN_SETUP(call, name)                                      \
  InstallContextFunction(isolate, context, #name, Builtin::kConsole##call, id, \
                         args.at(1));
  CONSOLE_METHOD_LIST(CONSOLE_BUILTIN_SETUP)
#undef CONSOLE_BUILTIN_SETUP
  InstallContextFunction(isolate, context, "time", Builtin::kConsoleTime, id,
                         args.at(1));
  InstallContextFunction(isolate, context, "timeEnd", Builtin::kConsoleTimeEnd,
                         id, args.at(1));
  InstallContextFunction(isolate, context, "timeStamp",
                         Builtin::kConsoleTimeStamp, id, args.at(1));

  return *context;
}

#undef CONSOLE_METHOD_LIST

}  // namespace internal
}  // namespace v8
