/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "subcontext.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <selinux/android.h>

#include "action.h"
#include "system/core/init/subcontext.pb.h"
#include "util.h"

using android::base::GetExecutablePath;
using android::base::Join;
using android::base::Socketpair;
using android::base::Split;
using android::base::StartsWith;
using android::base::unique_fd;

namespace android {
namespace init {

const std::string kInitContext = "u:r:init:s0";
const std::string kVendorContext = "u:r:vendor_init:s0";

namespace {

constexpr size_t kBufferSize = 4096;

Result<std::string> ReadMessage(int socket) {
    char buffer[kBufferSize] = {};
    auto result = TEMP_FAILURE_RETRY(recv(socket, buffer, sizeof(buffer), 0));
    if (result <= 0) {
        return ErrnoError();
    }
    return std::string(buffer, result);
}

template <typename T>
Result<Success> SendMessage(int socket, const T& message) {
    std::string message_string;
    if (!message.SerializeToString(&message_string)) {
        return Error() << "Unable to serialize message";
    }

    if (message_string.size() > kBufferSize) {
        return Error() << "Serialized message too long to send";
    }

    if (auto result =
            TEMP_FAILURE_RETRY(send(socket, message_string.c_str(), message_string.size(), 0));
        result != static_cast<long>(message_string.size())) {
        return ErrnoError() << "send() failed to send message contents";
    }
    return Success();
}

class SubcontextProcess {
  public:
    SubcontextProcess(const KeywordFunctionMap* function_map, std::string context, int init_fd)
        : function_map_(function_map), context_(std::move(context)), init_fd_(init_fd){};
    void MainLoop();

  private:
    void RunCommand(const SubcontextCommand::ExecuteCommand& execute_command,
                    SubcontextReply::ResultMessage* result_message) const;

    const KeywordFunctionMap* function_map_;
    const std::string context_;
    const int init_fd_;
};

void SubcontextProcess::RunCommand(const SubcontextCommand::ExecuteCommand& execute_command,
                                   SubcontextReply::ResultMessage* result_message) const {
    // Need to use ArraySplice instead of this code.
    auto args = std::vector<std::string>();
    for (const auto& string : execute_command.args()) {
        args.emplace_back(string);
    }

    auto map_result = function_map_->FindFunction(args);
    Result<Success> result;
    if (!map_result) {
        result = Error() << "Cannot find command: " << map_result.error();
    } else {
        result = RunBuiltinFunction(map_result->second, args, context_);
    }

    if (result) {
        result_message->set_success(true);
    } else {
        result_message->set_success(false);
        result_message->set_error_string(result.error_string());
        result_message->set_error_errno(result.error_errno());
    }
}

void SubcontextProcess::MainLoop() {
    pollfd ufd[1];
    ufd[0].events = POLLIN;
    ufd[0].fd = init_fd_;

    while (true) {
        ufd[0].revents = 0;
        int nr = TEMP_FAILURE_RETRY(poll(ufd, arraysize(ufd), -1));
        if (nr == 0) continue;
        if (nr < 0) {
            PLOG(FATAL) << "poll() of subcontext socket failed, continuing";
        }

        auto init_message = ReadMessage(init_fd_);
        if (!init_message) {
            LOG(FATAL) << "Could not read message from init: " << init_message.error();
        }

        auto subcontext_command = SubcontextCommand();
        if (!subcontext_command.ParseFromString(*init_message)) {
            LOG(FATAL) << "Unable to parse message from init";
        }

        auto reply = SubcontextReply();
        switch (subcontext_command.command_case()) {
            case SubcontextCommand::kExecuteCommand: {
                RunCommand(subcontext_command.execute_command(), reply.mutable_result());
                break;
            }
            default:
                LOG(FATAL) << "Unknown message type from init: "
                           << subcontext_command.command_case();
        }

        if (auto result = SendMessage(init_fd_, reply); !result) {
            LOG(FATAL) << "Failed to send message to init: " << result.error();
        }
    }
}

}  // namespace

int SubcontextMain(int argc, char** argv, const KeywordFunctionMap* function_map) {
    if (argc < 4) LOG(FATAL) << "Fewer than 4 args specified to subcontext (" << argc << ")";

    auto context = std::string(argv[2]);
    auto init_fd = std::atoi(argv[3]);

    auto subcontext_process = SubcontextProcess(function_map, context, init_fd);
    subcontext_process.MainLoop();
    return 0;
}

void Subcontext::Fork() {
    unique_fd subcontext_socket;
    if (!Socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, &socket_, &subcontext_socket)) {
        LOG(FATAL) << "Could not create socket pair to communicate to subcontext";
        return;
    }

    auto result = fork();

    if (result == -1) {
        LOG(FATAL) << "Could not fork subcontext";
    } else if (result == 0) {
        socket_.reset();

        // We explicitly do not use O_CLOEXEC here, such that we can reference this FD by number
        // in the subcontext process after we exec.
        int child_fd = dup(subcontext_socket);
        if (child_fd < 0) {
            PLOG(FATAL) << "Could not dup child_fd";
        }

        if (setexeccon(context_.c_str()) < 0) {
            PLOG(FATAL) << "Could not set execcon for '" << context_ << "'";
        }

        auto init_path = GetExecutablePath();
        auto child_fd_string = std::to_string(child_fd);
        const char* args[] = {init_path.c_str(), "subcontext", context_.c_str(),
                              child_fd_string.c_str(), nullptr};
        execv(init_path.data(), const_cast<char**>(args));

        PLOG(FATAL) << "Could not execv subcontext init";
    } else {
        subcontext_socket.reset();
        pid_ = result;
        LOG(INFO) << "Forked subcontext for '" << context_ << "' with pid " << pid_;
    }
}

void Subcontext::Restart() {
    LOG(ERROR) << "Restarting subcontext '" << context_ << "'";
    if (pid_) {
        kill(pid_, SIGKILL);
    }
    pid_ = 0;
    socket_.reset();
    Fork();
}

Result<Success> Subcontext::Execute(const std::vector<std::string>& args) {
    auto subcontext_command = SubcontextCommand();
    std::copy(
        args.begin(), args.end(),
        RepeatedPtrFieldBackInserter(subcontext_command.mutable_execute_command()->mutable_args()));

    if (auto result = SendMessage(socket_, subcontext_command); !result) {
        Restart();
        return ErrnoError() << "Failed to send message to subcontext";
    }

    auto subcontext_message = ReadMessage(socket_);
    if (!subcontext_message) {
        Restart();
        return Error() << "Failed to receive result from subcontext: " << subcontext_message.error();
    }

    auto subcontext_reply = SubcontextReply();
    if (!subcontext_reply.ParseFromString(*subcontext_message)) {
        Restart();
        return Error() << "Unable to parse message from subcontext";
    }

    switch (subcontext_reply.reply_case()) {
        case SubcontextReply::kResult: {
            auto result = subcontext_reply.result();
            if (result.success()) {
                return Success();
            } else {
                return ResultError(result.error_string(), result.error_errno());
            }
        }
        default:
            return Error() << "Unknown message type from subcontext: "
                           << subcontext_reply.reply_case();
    }
}

static std::vector<Subcontext> subcontexts;

std::vector<Subcontext>* InitializeSubcontexts() {
    static const char* const paths_and_secontexts[][2] = {
        //  TODO: Enable this once the SEPolicy is in place.
        //  {"/vendor", kVendorContext.c_str()},
    };
    for (const auto& [path_prefix, secontext] : paths_and_secontexts) {
        subcontexts.emplace_back(path_prefix, secontext);
    }
    return &subcontexts;
}

bool SubcontextChildReap(pid_t pid) {
    for (auto& subcontext : subcontexts) {
        if (subcontext.pid() == pid) {
            subcontext.Restart();
            return true;
        }
    }
    return false;
}

}  // namespace init
}  // namespace android
