// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/Server.h"
#include "server/init/InstanceLockCheck.h"

#include <fcntl.h>
#include <unistd.h>
#include <boost/filesystem.hpp>
#include <cstring>
#include <unordered_map>

#include "config/Config.h"
#include "index/archive/KnowhereResource.h"
#include "metrics/Metrics.h"
#include "scheduler/SchedInst.h"
#include "server/DBWrapper.h"
#include "server/grpc_impl/GrpcServer.h"
#include "server/init/CpuChecker.h"
#include "server/init/GpuChecker.h"
#include "server/init/StorageChecker.h"
#include "server/web_impl/WebServer.h"
#include "src/version.h"
//#include "storage/s3/S3ClientWrapper.h"
#include "tracing/TracerUtil.h"
#include "utils/Log.h"
#include "utils/LogUtil.h"
#include "utils/SignalHandler.h"
#include "utils/TimeRecorder.h"

#include "search/TaskInst.h"

namespace milvus {
namespace server {

Server&
Server::GetInstance() {
    static Server server;
    return server;
}

void
Server::Init(int64_t daemonized, const std::string& pid_filename, const std::string& config_filename) {
    daemonized_ = daemonized;
    pid_filename_ = pid_filename;
    config_filename_ = config_filename;
}

void
Server::Daemonize() {
    if (daemonized_ == 0) {
        return;
    }

    std::cout << "Milvus server run in daemonize mode";

    pid_t pid = 0;

    // Fork off the parent process
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Success: terminate parent
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // On success: The child process becomes session leader
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    // Ignore signal sent from child to parent process
    signal(SIGCHLD, SIG_IGN);

    // Fork off for the second time
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Terminate the parent
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Set new file permissions
    umask(0);

    // Change the working directory to root
    int ret = chdir("/");
    if (ret != 0) {
        return;
    }

    // Close all open fd
    for (int64_t fd = sysconf(_SC_OPEN_MAX); fd > 0; fd--) {
        close(fd);
    }

    std::cout << "Redirect stdin/stdout/stderr to /dev/null";

    // Redirect stdin/stdout/stderr to /dev/null
    stdin = fopen("/dev/null", "r");
    stdout = fopen("/dev/null", "w+");
    stderr = fopen("/dev/null", "w+");
    // Try to write PID of daemon to lockfile
    if (!pid_filename_.empty()) {
        pid_fd_ = open(pid_filename_.c_str(), O_RDWR | O_CREAT, 0640);
        if (pid_fd_ < 0) {
            std::cerr << "Can't open filename: " + pid_filename_ + ", Error: " + strerror(errno);
            exit(EXIT_FAILURE);
        }
        if (lockf(pid_fd_, F_TLOCK, 0) < 0) {
            std::cerr << "Can't lock filename: " + pid_filename_ + ", Error: " + strerror(errno);
            exit(EXIT_FAILURE);
        }

        std::string pid_file_context = std::to_string(getpid());
        ssize_t res = write(pid_fd_, pid_file_context.c_str(), pid_file_context.size());
        if (res != 0) {
            return;
        }
    }
}

Status
Server::Start() {
    if (daemonized_ != 0) {
        Daemonize();
    }

    try {
        /* Read config file */
        Status s = LoadConfig();
        if (!s.ok()) {
            std::cerr << "ERROR: Milvus server fail to load config file" << std::endl;
            return s;
        }

        Config& config = Config::GetInstance();

        std::string meta_uri;
        STATUS_CHECK(config.GetGeneralConfigMetaURI(meta_uri));
        if (meta_uri.length() > 6 && strcasecmp("sqlite", meta_uri.substr(0, 6).c_str()) == 0) {
            std::cout << "WARNNING: You are using SQLite as the meta data management, "
                         "which can't be used in production. Please change it to MySQL!"
                      << std::endl;
        }

        /* Init opentracing tracer from config */
        std::string tracing_config_path;
        s = config.GetTracingConfigJsonConfigPath(tracing_config_path);
        tracing_config_path.empty() ? tracing::TracerUtil::InitGlobal()
                                    : tracing::TracerUtil::InitGlobal(tracing_config_path);

        /* log path is defined in Config file, so InitLog must be called after LoadConfig */
        std::string time_zone;
        s = config.GetGeneralConfigTimezone(time_zone);
        if (!s.ok()) {
            std::cerr << "Fail to get server config timezone" << std::endl;
            return s;
        }

        if (time_zone.length() == 3) {
            time_zone = "CUT";
        } else {
            int time_bias = std::stoi(time_zone.substr(3, std::string::npos));
            if (time_bias == 0) {
                time_zone = "CUT";
            } else if (time_bias > 0) {
                time_zone = "CUT" + std::to_string(-time_bias);
            } else {
                time_zone = "CUT+" + std::to_string(-time_bias);
            }
        }

        if (setenv("TZ", time_zone.c_str(), 1) != 0) {
            return Status(SERVER_UNEXPECTED_ERROR, "Fail to setenv");
        }
        tzset();

        {
            std::unordered_map<std::string, int64_t> level_to_int{
                {"debug", 5}, {"info", 4}, {"warning", 3}, {"error", 2}, {"fatal", 1},
            };

            std::string level;
            bool trace_enable = false;
            bool debug_enable = false;
            bool info_enable = false;
            bool warning_enable = false;
            bool error_enable = false;
            bool fatal_enable = false;
            std::string logs_path;
            int64_t max_log_file_size = 0;
            int64_t delete_exceeds = 0;

            STATUS_CHECK(config.GetLogsLevel(level));
            switch (level_to_int[level]) {
                case 5:
                    debug_enable = true;
                case 4:
                    info_enable = true;
                case 3:
                    warning_enable = true;
                case 2:
                    error_enable = true;
                case 1:
                    fatal_enable = true;
                    break;
                default:
                    return Status(SERVER_UNEXPECTED_ERROR, "invalid log level");
            }

            STATUS_CHECK(config.GetLogsTraceEnable(trace_enable));
            STATUS_CHECK(config.GetLogsPath(logs_path));
            STATUS_CHECK(config.GetLogsMaxLogFileSize(max_log_file_size));
            STATUS_CHECK(config.GetLogsLogRotateNum(delete_exceeds));
            InitLog(trace_enable, debug_enable, info_enable, warning_enable, error_enable, fatal_enable, logs_path,
                    max_log_file_size, delete_exceeds);
        }

        bool cluster_enable = false;
        std::string cluster_role;
        STATUS_CHECK(config.GetClusterConfigEnable(cluster_enable));
        STATUS_CHECK(config.GetClusterConfigRole(cluster_role));

        if ((not cluster_enable) || cluster_role == "rw") {
            std::string db_path;
            STATUS_CHECK(config.GetStorageConfigPath(db_path));

            try {
                // True if a new directory was created, otherwise false.
                boost::filesystem::create_directories(db_path);
            } catch (...) {
                return Status(SERVER_UNEXPECTED_ERROR, "Cannot create db directory");
            }

            s = InstanceLockCheck::Check(db_path);
            if (!s.ok()) {
                if (not cluster_enable) {
                    std::cerr << "single instance lock db path failed." << s.message() << std::endl;
                } else {
                    std::cerr << cluster_role << " instance lock db path failed." << s.message() << std::endl;
                }
                return s;
            }

            bool wal_enable = false;
            STATUS_CHECK(config.GetWalConfigEnable(wal_enable));

            if (wal_enable) {
                std::string wal_path;
                STATUS_CHECK(config.GetWalConfigWalPath(wal_path));

                try {
                    // True if a new directory was created, otherwise false.
                    boost::filesystem::create_directories(wal_path);
                } catch (...) {
                    return Status(SERVER_UNEXPECTED_ERROR, "Cannot create wal directory");
                }
                s = InstanceLockCheck::Check(wal_path);
                if (!s.ok()) {
                    if (not cluster_enable) {
                        std::cerr << "single instance lock wal path failed." << s.message() << std::endl;
                    } else {
                        std::cerr << cluster_role << " instance lock wal path failed." << s.message() << std::endl;
                    }
                    return s;
                }
            }
        }

        // print version information
        LOG_SERVER_INFO_ << "Milvus " << BUILD_TYPE << " version: v" << MILVUS_VERSION << ", built at " << BUILD_TIME;
#ifdef MILVUS_GPU_VERSION
        LOG_SERVER_INFO_ << "GPU edition";
#else
        LOG_SERVER_INFO_ << "CPU edition";
#endif
        STATUS_CHECK(StorageChecker::CheckStoragePermission());
        STATUS_CHECK(CpuChecker::CheckCpuInstructionSet());
#ifdef MILVUS_GPU_VERSION
        STATUS_CHECK(GpuChecker::CheckGpuEnvironment());
#endif
        /* record config and hardware information into log */
        LogConfigInFile(config_filename_);
        LogCpuInfo();
        LogConfigInMem();

        server::Metrics::GetInstance().Init();
        server::SystemInfo::GetInstance().Init();

        return StartService();
    } catch (std::exception& ex) {
        std::string str = "Milvus server encounter exception: " + std::string(ex.what());
        return Status(SERVER_UNEXPECTED_ERROR, str);
    }
}

void
Server::Stop() {
    std::cerr << "Milvus server is going to shutdown ..." << std::endl;

    /* Unlock and close lockfile */
    if (pid_fd_ != -1) {
        int ret = lockf(pid_fd_, F_ULOCK, 0);
        if (ret != 0) {
            std::cerr << "ERROR: Can't lock file: " << strerror(errno) << std::endl;
            exit(0);
        }
        ret = close(pid_fd_);
        if (ret != 0) {
            std::cerr << "ERROR: Can't close file: " << strerror(errno) << std::endl;
            exit(0);
        }
    }

    /* delete lockfile */
    if (!pid_filename_.empty()) {
        int ret = unlink(pid_filename_.c_str());
        if (ret != 0) {
            std::cerr << "ERROR: Can't unlink file: " << strerror(errno) << std::endl;
            exit(0);
        }
    }

    StopService();

    std::cerr << "Milvus server exit..." << std::endl;
}

Status
Server::LoadConfig() {
    Config& config = Config::GetInstance();
    Status s = config.LoadConfigFile(config_filename_);
    if (!s.ok()) {
        std::cerr << s.message() << std::endl;
        return s;
    }

    s = config.ValidateConfig();
    if (!s.ok()) {
        std::cerr << "Config check fail: " << s.message() << std::endl;
        return s;
    }
    return milvus::Status::OK();
}

Status
Server::StartService() {
    Status stat;
    stat = engine::KnowhereResource::Initialize();
    if (!stat.ok()) {
        LOG_SERVER_ERROR_ << "KnowhereResource initialize fail: " << stat.message();
        goto FAIL;
    }

    scheduler::StartSchedulerService();

    stat = DBWrapper::GetInstance().StartService();
    if (!stat.ok()) {
        LOG_SERVER_ERROR_ << "DBWrapper start service fail: " << stat.message();
        goto FAIL;
    }

    grpc::GrpcServer::GetInstance().Start();
    web::WebServer::GetInstance().Start();

    // stat = storage::S3ClientWrapper::GetInstance().StartService();
    // if (!stat.ok()) {
    //     LOG_SERVER_ERROR_ << "S3Client start service fail: " << stat.message();
    //     goto FAIL;
    // }

    //    search::TaskInst::GetInstance().Start();

    return Status::OK();
FAIL:
    std::cerr << "Milvus initializes fail: " << stat.message() << std::endl;
    return stat;
}

void
Server::StopService() {
    //    search::TaskInst::GetInstance().Stop();
    // storage::S3ClientWrapper::GetInstance().StopService();
    web::WebServer::GetInstance().Stop();
    grpc::GrpcServer::GetInstance().Stop();
    DBWrapper::GetInstance().StopService();
    scheduler::StopSchedulerService();
    engine::KnowhereResource::Finalize();
}

}  // namespace server
}  // namespace milvus
