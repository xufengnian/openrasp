/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

extern "C"
{
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_openrasp.h"
#include "php_scandir.h"
}
#include <sstream>
#include <fstream>
#include "openrasp_v8.h"
#include "js/openrasp_v8_js.h"
#include "openrasp_ini.h"

using namespace openrasp;

ZEND_DECLARE_MODULE_GLOBALS(openrasp_v8)

openrasp_v8_process_globals openrasp::process_globals;

void openrasp_load_plugins();
static bool init_isolate();
static bool shutdown_isolate();

unsigned char openrasp_check(const char *c_type, zval *z_params)
{
    if (!init_isolate())
    {
        return 0;
    }
    v8::Isolate *isolate = OPENRASP_V8_G(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handlescope(isolate);
    v8::Local<v8::Context> context = OPENRASP_V8_G(context).Get(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch;
    v8::Local<v8::Function> check = OPENRASP_V8_G(check).Get(isolate);
    v8::Local<v8::Value> type = V8STRING_N(c_type).ToLocalChecked();
    v8::Local<v8::Value> params = zval_to_v8val(z_params, isolate);
    v8::Local<v8::Object> request_context = OPENRASP_V8_G(request_context).Get(isolate);
    v8::Local<v8::Value> argv[]{type, params, request_context};

    v8::Local<v8::Value> rst;
    {
        TimeoutTask *task = new TimeoutTask(isolate, openrasp_ini.timeout_ms);
        std::lock_guard<std::timed_mutex> lock(task->GetMtx());
        process_globals.v8_platform->CallOnBackgroundThread(task, v8::Platform::kShortRunningTask);
        bool avoidwarning = check->Call(context, check, 3, argv).ToLocal(&rst);
    }
    if (rst.IsEmpty())
    {
        if (try_catch.Message().IsEmpty())
        {
            v8::Local<v8::Function> console_log = context->Global()
                                                      ->Get(context, V8STRING_I("console").ToLocalChecked())
                                                      .ToLocalChecked()
                                                      .As<v8::Object>()
                                                      ->Get(context, V8STRING_I("log").ToLocalChecked())
                                                      .ToLocalChecked()
                                                      .As<v8::Function>();
            v8::Local<v8::Object> message = v8::Object::New(isolate);
            message->Set(V8STRING_N("message").ToLocalChecked(), V8STRING_N("Javascript plugin execution timeout.").ToLocalChecked());
            message->Set(V8STRING_N("type").ToLocalChecked(), type);
            message->Set(V8STRING_N("params").ToLocalChecked(), params);
            message->Set(V8STRING_N("context").ToLocalChecked(), request_context);
            bool avoidwarning = console_log->Call(context, console_log, 1, reinterpret_cast<v8::Local<v8::Value> *>(&message)).IsEmpty();
        }
        else
        {
            std::stringstream stream;
            v8error_to_stream(isolate, try_catch, stream);
            std::string error = stream.str();
            plugin_info(error.c_str(), error.length());
        }
        return 0;
    }
    if (!rst->IsArray())
    {
        return 0;
    }
    v8::Local<v8::String> key_action = OPENRASP_V8_G(key_action).Get(isolate);
    v8::Local<v8::String> key_message = OPENRASP_V8_G(key_message).Get(isolate);
    v8::Local<v8::String> key_name = OPENRASP_V8_G(key_name).Get(isolate);
    v8::Local<v8::String> key_confidence = OPENRASP_V8_G(key_confidence).Get(isolate);

    v8::Local<v8::Array> arr = rst.As<v8::Array>();
    int len = arr->Length();
    bool is_block = false;
    for (int i = 0; i < len; i++)
    {
        v8::Local<v8::Object> item = arr->Get(i).As<v8::Object>();
        v8::Local<v8::Value> v8_action = item->Get(key_action);
        if (!v8_action->IsString())
        {
            continue;
        }
        int action_hash = v8_action->ToString()->GetIdentityHash();
        if (OPENRASP_V8_G(action_hash_ignore) == action_hash)
        {
            continue;
        }
        is_block = is_block || OPENRASP_V8_G(action_hash_block) == action_hash;

        v8::Local<v8::Value> v8_message = item->Get(key_message);
        v8::Local<v8::Value> v8_name = item->Get(key_name);
        v8::Local<v8::Value> v8_confidence = item->Get(key_confidence);
        v8::String::Utf8Value utf_action(v8_action);
        v8::String::Utf8Value utf_message(v8_message);
        v8::String::Utf8Value utf_name(v8_name);

        zval result;
        array_init(&result);
        add_assoc_stringl(&result, "attack_type", const_cast<char *>(c_type), strlen(c_type));
        add_assoc_stringl(&result, "intercept_state", *utf_action, utf_action.length());
        add_assoc_stringl(&result, "plugin_message", *utf_message, utf_message.length());
        add_assoc_stringl(&result, "plugin_name", *utf_name, utf_name.length());
        add_assoc_long(&result, "plugin_confidence", v8_confidence->Int32Value());
        add_assoc_zval(&result, "attack_params", z_params);
        Z_TRY_ADDREF_P(z_params);
        alarm_info(&result);
        zval_dtor(&result);
    }
    return is_block ? 1 : 0;
}

static void v8native_log(const v8::FunctionCallbackInfo<v8::Value> &info)
{

    for (int i = 0; i < info.Length(); i++)
    {
        v8::String::Utf8Value message(info[i]);
        plugin_info(*message, message.length());
    }
}

static void v8native_antlr4(const v8::FunctionCallbackInfo<v8::Value> &info)
{
#ifdef HAVE_NATIVE_ANTLR4
    static TokenizeErrorListener tokenizeErrorListener;
    if (info.Length() >= 1 && info[0]->IsString())
    {
        antlr4::ANTLRInputStream input(*v8::String::Utf8Value(info[0]));
        SQLLexer lexer(&input);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&tokenizeErrorListener);
        antlr4::CommonTokenStream output(&lexer);
        output.fill();
        auto tokens = output.getTokens();
        int length = tokens.size();
        v8::Isolate *isolate = info.GetIsolate();
        v8::Local<v8::Array> arr = v8::Array::New(isolate, length - 1);
        for (int i = 0; i < length - 1; i++)
        {
            v8::Local<v8::String> token;
            if (V8STRING_N(tokens[i]->getText().c_str()).ToLocal(&token))
            {
                arr->Set(i, token);
            }
        }
        info.GetReturnValue().Set(arr);
    }
#endif
}

intptr_t external_references[] = {
    reinterpret_cast<intptr_t>(v8native_log),
    reinterpret_cast<intptr_t>(v8native_antlr4),
    0,
};

static v8::StartupData init_js_snapshot()
{
    v8::SnapshotCreator creator(external_references);
    v8::Isolate *isolate = creator.GetIsolate();
#ifdef PHP_WIN32
    uintptr_t current_stack = reinterpret_cast<uintptr_t>(&current_stack);
    uintptr_t stack_limit = current_stack - 512 * 1024;
    stack_limit = stack_limit < current_stack ? stack_limit : sizeof(stack_limit);
    isolate->SetStackLimit(stack_limit);
#endif
    {
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        v8::TryCatch try_catch;
        v8::Local<v8::Object> global = context->Global();
        global->Set(V8STRING_I("global").ToLocalChecked(), global);
        v8::Local<v8::Function> log = v8::Function::New(isolate, v8native_log);
        v8::Local<v8::Object> v8_stdout = v8::Object::New(isolate);
        v8_stdout->Set(V8STRING_I("write").ToLocalChecked(), log);
        global->Set(V8STRING_I("stdout").ToLocalChecked(), v8_stdout);
        global->Set(V8STRING_I("stderr").ToLocalChecked(), v8_stdout);

#define MAKE_JS_SRC_PAIR(name) {{(const char *)name##_js, name##_js_len}, ZEND_TOSTR(name) ".js"}
        std::vector<std::pair<std::string, std::string>> js_src_list = {
            MAKE_JS_SRC_PAIR(console),
            MAKE_JS_SRC_PAIR(checkpoint),
            MAKE_JS_SRC_PAIR(error),
            MAKE_JS_SRC_PAIR(context),
            MAKE_JS_SRC_PAIR(sql_tokenize),
            MAKE_JS_SRC_PAIR(rasp),
        };
        for (auto js_src : js_src_list)
        {
            if (exec_script(isolate, context, std::move(js_src.first), std::move(js_src.second)).IsEmpty())
            {
                std::stringstream stream;
                v8error_to_stream(isolate, try_catch, stream);
                std::string error = stream.str();
                plugin_info(error.c_str(), error.length());
                openrasp_error(E_WARNING, PLUGIN_ERROR, _("Fail to initialize js plugin - %s"), error.c_str());
                return v8::StartupData{nullptr, 0};
            }
        }
#ifdef HAVE_NATIVE_ANTLR4
        v8::Local<v8::Function> sql_tokenize = v8::Function::New(isolate, v8native_antlr4);
        context->Global()
            ->Get(context, V8STRING_I("RASP").ToLocalChecked())
            .ToLocalChecked()
            .As<v8::Object>()
            ->Set(V8STRING_I("sql_tokenize").ToLocalChecked(), sql_tokenize);
#endif
        for (auto plugin_src : process_globals.plugin_src_list)
        {
            if (exec_script(isolate, context, "(function(){\n" + plugin_src.source + "\n})()", plugin_src.filename, -1).IsEmpty())
            {
                std::stringstream stream;
                v8error_to_stream(isolate, try_catch, stream);
                std::string error = stream.str();
                plugin_info(error.c_str(), error.length());
            }
        }
        creator.SetDefaultContext(context);
    }
    return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
}

static bool init_isolate()
{
    if (process_globals.is_initialized && !OPENRASP_V8_G(is_isolate_initialized))
    {
        if (!process_globals.v8_platform)
        {
#ifdef ZTS
            process_globals.v8_platform = v8::platform::CreateDefaultPlatform();
#else
            process_globals.v8_platform = v8::platform::CreateDefaultPlatform(1);
#endif
            v8::V8::InitializePlatform(process_globals.v8_platform);
        }
        OPENRASP_V8_G(create_params).array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        OPENRASP_V8_G(create_params).snapshot_blob = &process_globals.snapshot_blob;
        OPENRASP_V8_G(create_params).external_references = external_references;

        v8::Isolate *isolate = v8::Isolate::New(OPENRASP_V8_G(create_params));
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::String> key_action = V8STRING_I("action").ToLocalChecked();
        v8::Local<v8::String> key_message = V8STRING_I("message").ToLocalChecked();
        v8::Local<v8::String> key_name = V8STRING_I("name").ToLocalChecked();
        v8::Local<v8::String> key_confidence = V8STRING_I("confidence").ToLocalChecked();
        v8::Local<v8::Object> RASP = context->Global()
                                         ->Get(context, V8STRING_I("RASP").ToLocalChecked())
                                         .ToLocalChecked()
                                         .As<v8::Object>();
        v8::Local<v8::Function> check = RASP->Get(context, V8STRING_I("check").ToLocalChecked())
                                            .ToLocalChecked()
                                            .As<v8::Function>();

        OPENRASP_V8_G(isolate) = isolate;
        OPENRASP_V8_G(context).Reset(isolate, context);
        OPENRASP_V8_G(key_action).Reset(isolate, key_action);
        OPENRASP_V8_G(key_message).Reset(isolate, key_message);
        OPENRASP_V8_G(key_name).Reset(isolate, key_name);
        OPENRASP_V8_G(key_confidence).Reset(isolate, key_confidence);
        OPENRASP_V8_G(RASP).Reset(isolate, RASP);
        OPENRASP_V8_G(check).Reset(isolate, check);
        OPENRASP_V8_G(request_context).Reset(isolate, RequestContext::New(isolate));

        OPENRASP_V8_G(action_hash_ignore) = V8STRING_N("ignore").ToLocalChecked()->GetIdentityHash();
        OPENRASP_V8_G(action_hash_log) = V8STRING_N("log").ToLocalChecked()->GetIdentityHash();
        OPENRASP_V8_G(action_hash_block) = V8STRING_N("block").ToLocalChecked()->GetIdentityHash();

        OPENRASP_V8_G(is_isolate_initialized) = true;
    }
    return OPENRASP_V8_G(is_isolate_initialized);
}

static bool shutdown_isolate()
{
    if (OPENRASP_V8_G(is_isolate_initialized))
    {
        OPENRASP_V8_G(isolate)->Dispose();
        delete OPENRASP_V8_G(create_params).array_buffer_allocator;
        OPENRASP_V8_G(is_isolate_initialized) = false;
    }
    return true;
}

void openrasp_load_plugins()
{
    std::vector<openrasp_v8_plugin_src> plugin_src_list;
    std::string plugin_path(std::string(openrasp_ini.root_dir) + DEFAULT_SLASH + std::string("plugins"));
    dirent **ent = nullptr;
    int n_plugin = php_scandir(plugin_path.c_str(), &ent, nullptr, nullptr);
    for (int i = 0; i < n_plugin; i++)
    {
        const char *p = strrchr(ent[i]->d_name, '.');
        if (p != nullptr && strcasecmp(p, ".js") == 0)
        {
            std::string filename(ent[i]->d_name);
            std::string filepath(plugin_path + DEFAULT_SLASH + filename);
            struct stat sb;
            if (VCWD_STAT(filepath.c_str(), &sb) == 0 && (sb.st_mode & S_IFREG) != 0)
            {
                std::ifstream file(filepath);
                std::streampos beg = file.tellg();
                file.seekg(0, std::ios::end);
                std::streampos end = file.tellg();
                file.seekg(0, std::ios::beg);
                // plugin file size limitation: 10 MB
                if (10 * 1024 * 1024 >= end - beg)
                {
                    std::string source((std::istreambuf_iterator<char>(file)),
                                       std::istreambuf_iterator<char>());
                    plugin_src_list.push_back(openrasp_v8_plugin_src{filename, source});
                }
                else
                {
                    openrasp_error(E_WARNING, CONFIG_ERROR, _("Ignored Javascript plugin file '%s', as it exceeds 10 MB in file size."), filename.c_str());
                }
            }
        }
        free(ent[i]);
    }
    free(ent);
    process_globals.plugin_src_list = std::move(plugin_src_list);
}

PHP_GINIT_FUNCTION(openrasp_v8)
{
#ifdef ZTS
    new (openrasp_v8_globals) _zend_openrasp_v8_globals;
#endif
}

PHP_GSHUTDOWN_FUNCTION(openrasp_v8)
{
    shutdown_isolate();
#ifdef ZTS
    openrasp_v8_globals->~_zend_openrasp_v8_globals();
#endif
}

PHP_MINIT_FUNCTION(openrasp_v8)
{
    ZEND_INIT_MODULE_GLOBALS(openrasp_v8, PHP_GINIT(openrasp_v8), PHP_GSHUTDOWN(openrasp_v8));

    openrasp_load_plugins();
    if (process_globals.plugin_src_list.size() <= 0)
    {
        return SUCCESS;
    }

    // It can be called multiple times,
    // but intern code initializes v8 only once
    v8::V8::Initialize();

    v8::Platform *platform = v8::platform::CreateDefaultPlatform();
    v8::V8::InitializePlatform(platform);
    process_globals.snapshot_blob = init_js_snapshot();
    v8::V8::ShutdownPlatform();
    delete platform;

    if (process_globals.snapshot_blob.data == nullptr ||
        process_globals.snapshot_blob.raw_size <= 0)
    {
        return FAILURE;
    }

    process_globals.is_initialized = true;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(openrasp_v8)
{
    ZEND_SHUTDOWN_MODULE_GLOBALS(openrasp_v8, PHP_GSHUTDOWN(openrasp_v8));

    if (process_globals.is_initialized)
    {
        // Disposing v8 is permanent, it cannot be reinitialized,
        // it should generally not be necessary to dispose v8 before exiting a process,
        // so skip this step for module graceful reload
        // v8::V8::Dispose();
        if (process_globals.v8_platform)
        {
            v8::V8::ShutdownPlatform();
            delete process_globals.v8_platform;
            process_globals.v8_platform = nullptr;
        }
        delete[] process_globals.snapshot_blob.data;
        process_globals.snapshot_blob.data = nullptr;
        process_globals.is_initialized = false;
    }
    return SUCCESS;
}
