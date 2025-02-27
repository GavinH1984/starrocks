// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/service/http_service.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "service/http_service.h"

#include "gutil/stl_util.h"
#include "http/action/checksum_action.h"
#include "http/action/compaction_action.h"
#include "http/action/health_action.h"
#include "http/action/list_workgroup_action.h"
#include "http/action/meta_action.h"
#include "http/action/metrics_action.h"
#include "http/action/pprof_actions.h"
#include "http/action/reload_tablet_action.h"
#include "http/action/restore_tablet_action.h"
#include "http/action/runtime_filter_cache_action.h"
#include "http/action/snapshot_action.h"
#include "http/action/stream_load.h"
#include "http/action/update_config_action.h"
#include "http/default_path_handlers.h"
#include "http/download_action.h"
#include "http/ev_http_server.h"
#include "http/http_method.h"
#include "http/monitor_action.h"
#include "http/web_page_handler.h"
#include "runtime/exec_env.h"
#include "runtime/load_path_mgr.h"
#include "util/file_utils.h"
#include "util/starrocks_metrics.h"

namespace starrocks {

HttpService::HttpService(ExecEnv* env, int port, int num_threads)
        : _env(env),
          _ev_http_server(new EvHttpServer(port, num_threads)),
          _web_page_handler(new WebPageHandler(_ev_http_server.get())) {}

HttpService::~HttpService() {
    _ev_http_server.reset();
    _web_page_handler.reset();
    STLDeleteElements(&_http_handlers);
}

Status HttpService::start() {
    add_default_path_handlers(_web_page_handler.get(), _env->process_mem_tracker());

    // register load
    StreamLoadAction* stream_load_action = new StreamLoadAction(_env);
    _ev_http_server->register_handler(HttpMethod::PUT, "/api/{db}/{table}/_stream_load", stream_load_action);
    _http_handlers.emplace_back(stream_load_action);

    // register download action
    std::vector<std::string> allow_paths;
    for (auto& path : _env->store_paths()) {
        allow_paths.emplace_back(path.path);
    }
    DownloadAction* download_action = new DownloadAction(_env, allow_paths);
    _ev_http_server->register_handler(HttpMethod::HEAD, "/api/_download_load", download_action);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/_download_load", download_action);
    _http_handlers.emplace_back(download_action);

    DownloadAction* tablet_download_action = new DownloadAction(_env, allow_paths);
    _ev_http_server->register_handler(HttpMethod::HEAD, "/api/_tablet/_download", tablet_download_action);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/_tablet/_download", tablet_download_action);
    _http_handlers.emplace_back(tablet_download_action);

    DownloadAction* error_log_download_action =
            new DownloadAction(_env, _env->load_path_mgr()->get_load_error_file_dir());
    _ev_http_server->register_handler(HttpMethod::GET, "/api/_load_error_log", error_log_download_action);
    _ev_http_server->register_handler(HttpMethod::HEAD, "/api/_load_error_log", error_log_download_action);
    _http_handlers.emplace_back(error_log_download_action);

    // Register BE health action
    HealthAction* health_action = new HealthAction(_env);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/health", health_action);
    _http_handlers.emplace_back(health_action);

    // register pprof actions
    if (!config::pprof_profile_dir.empty()) {
        FileUtils::create_dir(config::pprof_profile_dir);
    }

    HeapAction* heap_action = new HeapAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/heap", heap_action);
    _http_handlers.emplace_back(heap_action);

    GrowthAction* growth_action = new GrowthAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/growth", growth_action);
    _http_handlers.emplace_back(growth_action);

    ProfileAction* profile_action = new ProfileAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/profile", profile_action);
    _http_handlers.emplace_back(profile_action);

    PmuProfileAction* pmu_profile_action = new PmuProfileAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/pmuprofile", pmu_profile_action);
    _http_handlers.emplace_back(pmu_profile_action);

    ContentionAction* contention_action = new ContentionAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/contention", contention_action);
    _http_handlers.emplace_back(contention_action);

    CmdlineAction* cmdline_action = new CmdlineAction();
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/cmdline", cmdline_action);
    _http_handlers.emplace_back(cmdline_action);

    SymbolAction* symbol_action = new SymbolAction(_env->bfd_parser());
    _ev_http_server->register_handler(HttpMethod::GET, "/pprof/symbol", symbol_action);
    _ev_http_server->register_handler(HttpMethod::HEAD, "/pprof/symbol", symbol_action);
    _ev_http_server->register_handler(HttpMethod::POST, "/pprof/symbol", symbol_action);
    _http_handlers.emplace_back(symbol_action);

    // register metrics
    {
        auto action = new MetricsAction(StarRocksMetrics::instance()->metrics());
        _ev_http_server->register_handler(HttpMethod::GET, "/metrics", action);
        _http_handlers.emplace_back(action);
    }

    MetaAction* meta_action = new MetaAction(HEADER);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/meta/header/{tablet_id}/{schema_hash}", meta_action);
    _http_handlers.emplace_back(meta_action);

#ifndef BE_TEST
    // Register BE checksum action
    ChecksumAction* checksum_action = new ChecksumAction(_env);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/checksum", checksum_action);
    _http_handlers.emplace_back(checksum_action);

    // Register BE reload tablet action
    ReloadTabletAction* reload_tablet_action = new ReloadTabletAction(_env);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/reload_tablet", reload_tablet_action);
    _http_handlers.emplace_back(reload_tablet_action);

    RestoreTabletAction* restore_tablet_action = new RestoreTabletAction(_env);
    _ev_http_server->register_handler(HttpMethod::POST, "/api/restore_tablet", restore_tablet_action);
    _http_handlers.emplace_back(restore_tablet_action);

    // Register BE snapshot action
    SnapshotAction* snapshot_action = new SnapshotAction(_env);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/snapshot", snapshot_action);
    _http_handlers.emplace_back(snapshot_action);
#endif

    // 2 compaction actions
    CompactionAction* show_compaction_action = new CompactionAction(CompactionActionType::SHOW_INFO);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/compaction/show", show_compaction_action);
    _http_handlers.emplace_back(show_compaction_action);

    CompactionAction* run_compaction_action = new CompactionAction(CompactionActionType::RUN_COMPACTION);
    _ev_http_server->register_handler(HttpMethod::POST, "/api/compact", run_compaction_action);
    _http_handlers.emplace_back(run_compaction_action);

    UpdateConfigAction* update_config_action = new UpdateConfigAction(_env);
    _ev_http_server->register_handler(HttpMethod::POST, "/api/update_config", update_config_action);
    _http_handlers.emplace_back(update_config_action);

    ListWorkGroupAction* list_workgroup_action = new ListWorkGroupAction(_env);
    _ev_http_server->register_handler(HttpMethod::POST, "/api/list_resource_groups", list_workgroup_action);
    _http_handlers.emplace_back(list_workgroup_action);

    RuntimeFilterCacheAction* runtime_filter_cache_action = new RuntimeFilterCacheAction(_env);
    _ev_http_server->register_handler(HttpMethod::GET, "/api/runtime_filter_cache/{action}",
                                      runtime_filter_cache_action);
    _ev_http_server->register_handler(HttpMethod::PUT, "/api/runtime_filter_cache/{action}",
                                      runtime_filter_cache_action);
    _http_handlers.emplace_back(runtime_filter_cache_action);

    RETURN_IF_ERROR(_ev_http_server->start());
    return Status::OK();
}

} // namespace starrocks
