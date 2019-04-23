// Copyright 2019 ByteDance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

#include <cuda_runtime.h>

#include "operations.h"

namespace byteps {
namespace common {

bool RunReduceLoopOnce() {
    QueueType this_op = REDUCE;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto reduce_stream =  BytePSGlobal::GetReduceStream();
    if (q->pendingSize() > 0) {
        auto task = q->getTask();
        BPS_CHECK(task->tensor);

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->tensor->size();
            auto cpubuff = task->cpubuff;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            BPS_LOG(TRACE) << name << ": Copy from GPU to CPU (reduce), size=" << len;
            CUDA_CALL(cudaMemcpyAsync(cpubuff, task->tensor->data(), len, cudaMemcpyDeviceToHost, *reduce_stream));
            CUDA_CALL(cudaStreamSynchronize(*reduce_stream));
            BPS_LOG(TRACE) << name << " reduce succeed";
        }

        if (task->last_op != this_op) { // TODO: should check the boundary
            BPS_LOG(TRACE) << "Finish reducing tensor: " << task->tensor_name
                           << ", passing it to the next queue " << this_op+1;
            BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(this_op+1))->addTask(task);
        } else {
            task->callback(Status::OK());
        }
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPushLoopOnce() {
    QueueType this_op = PUSH;
    auto q = BytePSGlobal::GetScheduledQueue(PUSH);
    if (q->pendingSize() > 0) {
        // TODO: allow merging
        auto task = q->getTask();

        char* data;
        if (task->device != CPU_DEVICE_ID) {
            BPS_CHECK(task->cpubuff);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff));
        } else {
            BPS_CHECK(task->tensor);
            data = const_cast<char*> (static_cast<const char*> (task->tensor->data()));
        }

        // get metadata
        size_t size = task->tensor->size();
        const int dtype = task->tensor->dtype();

        // false means not to delete data when SArray is deleted
        ps::SArray<char> vals(data, size, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, size);
        BytePSGlobal::GetPS()->ZPush(
            pskv.keys, vals, pskv.lens, cmd,
            [task, this_op]() {
                if (task->last_op != this_op) {
                    BPS_LOG(TRACE) << "Finish pushing tensor: " << task->tensor_name
                                   << ", passing it to the next queue " << this_op+1;
                    BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(this_op+1))->addTask(task);
                } else {
                    BPS_LOG(TRACE) << "Finish pushing tensor: " << task->tensor_name
                                   << ", invoking callback.";
                    task->callback(Status::OK());
                }
            }
        );
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPullLoopOnce() {
    QueueType this_op = PULL;
    auto q = BytePSGlobal::GetScheduledQueue(PULL);
    if (q->pendingSize() > 0) {
        // TODO: allow merging
        auto task = q->getTask();

        char* data;
        if (task->device != CPU_DEVICE_ID) {
            BPS_CHECK(task->cpubuff);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff));
        } else {
            BPS_CHECK(task->output);
            data = const_cast<char*> (static_cast<const char*> (task->output->data()));
        }

        // get metadata
        size_t size = task->output->size();
        const int dtype = task->output->dtype();

        // false means not to delete data when SArray is deleted
        auto vals = new ps::SArray<char>(data, size, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, size);
        // issue pull
        BytePSGlobal::GetPS()->ZPull(
            pskv.keys, vals, &pskv.lens, cmd,
            [vals, task, this_op]() {
                delete vals;
                if (task->last_op != this_op) {
                    BPS_LOG(TRACE) << "Finish pulling tensor: " << task->tensor_name
                                   << ", passing it the next queue " << this_op+1;
                    BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(this_op+1))->addTask(task);
                } else {
                    task->callback(Status::OK());
                }
            });
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunBroadcastLoopOnce() {
    QueueType this_op = BROADCAST;
    auto q = BytePSGlobal::GetScheduledQueue(BROADCAST);
    auto broadcast_stream = BytePSGlobal::GetBroadcastStream();
    if (q->pendingSize() > 0) {
        auto task = q->getTask();
        BPS_CHECK(task->output);

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->output->size();
            auto cpubuff = task->cpubuff;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            BPS_LOG(TRACE) << name << ": Copy from CPU to GPU (broadcast), size=" << len;
            char* gpu_addr = const_cast<char*> (static_cast<const char*> (task->output->data()));
            CUDA_CALL(cudaMemcpyAsync(gpu_addr, cpubuff, len, cudaMemcpyHostToDevice, *broadcast_stream));
            CUDA_CALL(cudaStreamSynchronize(*broadcast_stream));
            BPS_LOG(TRACE) << name << " broadcast succeed";
        }

        BPS_LOG(TRACE) << "Finish broadcasting tensor: " << task->tensor_name;

        BPS_CHECK_EQ(this_op, QueueNum-1) << "this should the last op";
        task->callback(Status::OK());
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

void ReduceLoop() {
    while (RunReduceLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PushLoop() {
    while (RunPushLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PullLoop() {
    while (RunPullLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void BroadcastLoop() {
    while (RunBroadcastLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

extern "C" {

void byteps_init() {
    BytePSGlobal::Init();
    LoopFunction func[ThreadNum] = {ReduceLoop, PushLoop, PullLoop, BroadcastLoop};
    BytePSGlobal::Start(func);
    return;
}

void byteps_shutdown() {
    BytePSGlobal::Shutdown();
    BPS_LOG(TRACE) << "BytePS is shutdown.";
    return;
}

int byteps_rank() {
    return BytePSGlobal::GetRank();
}

int byteps_local_rank() {
    return BytePSGlobal::GetLocalRank();
}

int byteps_size() {
    return BytePSGlobal::GetSize();
}

int byteps_local_size() {
    return BytePSGlobal::GetLocalSize();
}

} // extern "C"

Status CheckInitialized() {
    return BytePSGlobal::CheckInit();
}

Status EnqueueTensorReduce(std::shared_ptr<OpContext> context,
                        std::shared_ptr<Tensor> input,
                        std::shared_ptr<ReadyEvent> ready_event,
                        const std::string &name, ps::Key key,
                        const int device, const int priority, const int version,
                        StatusCallback callback, void* cpubuff, QueueType last_op,
                        ps::SArray<ps::Key> keys, ps::SArray<int> lens) {
    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->key = key;
    e->context = context;
    e->tensor = input;
    e->output = NULL;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;

    BPS_CHECK(e->tensor);
    e->keys = keys;
    e->lens = lens;

    e->cpubuff = cpubuff;
    e->last_op = last_op;

    BPS_LOG(TRACE) << "EnqueueTensorReduce: " << e->tensor_name
                   << ", key=" << e->key
                   << ", size=" << e->tensor->size()
                   << ", device=" << device
                   << ", last_op=" << last_op;
    BytePSGlobal::GetScheduledQueue(REDUCE)->addTask(e);
    return Status::OK();
}

Status EnqueueTensorPush(std::shared_ptr<OpContext> context,
                        std::shared_ptr<Tensor> input,
                        std::shared_ptr<ReadyEvent> ready_event,
                        const std::string &name, ps::Key key,
                        const int device, const int priority, const int version,
                        StatusCallback callback, void* cpubuff, QueueType last_op,
                        ps::SArray<ps::Key> keys, ps::SArray<int> lens) {
    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->key = key;
    e->context = context;
    e->tensor = input;
    e->output = NULL;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;

    BPS_CHECK(e->tensor);
    e->keys = keys;
    e->lens = lens;

    e->cpubuff = cpubuff;
    e->last_op = last_op;

    BPS_LOG(TRACE) << "EnqueueTensorPush: " << e->tensor_name
                   << ", key=" << e->key
                   << ", size=" << e->tensor->size()
                   << ", device=" << device
                   << ", last_op=" << last_op;
    BytePSGlobal::GetScheduledQueue(PUSH)->addTask(e);
    return Status::OK();
}

Status EnqueueTensorPull(std::shared_ptr<OpContext> context,
                        std::shared_ptr<Tensor> output,
                        std::shared_ptr<ReadyEvent> ready_event,
                        const std::string &name, ps::Key key,
                        const int device, const int priority, const int version,
                        StatusCallback callback, void* cpubuff, QueueType last_op,
                        ps::SArray<ps::Key> keys, ps::SArray<int> lens) {
    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->key = key;
    e->context = context;
    e->tensor = NULL;
    e->output = output;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;

    BPS_CHECK(e->output);
    e->keys = keys;
    e->lens = lens;

    e->cpubuff = cpubuff;
    e->last_op = last_op;

    BPS_LOG(TRACE) << "EnqueueTensorPull: " << e->tensor_name
                   << ", key=" << e->key
                   << ", size=" << e->output->size()
                   << ", device=" << device
                   << ", last_op=" << last_op;
    BytePSGlobal::GetScheduledQueue(PULL)->addTask(e);
    return Status::OK();
}

Status EnqueueTensorBroadcast(std::shared_ptr<OpContext> context,
                        std::shared_ptr<Tensor> output,
                        std::shared_ptr<ReadyEvent> ready_event,
                        const std::string &name, ps::Key key,
                        const int device, const int priority, const int version,
                        StatusCallback callback, void* cpubuff, QueueType last_op,
                        ps::SArray<ps::Key> keys, ps::SArray<int> lens) {
    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->key = key;
    e->context = context;
    e->tensor = NULL;
    e->output = output;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;

    BPS_CHECK(e->output);
    e->keys = keys;
    e->lens = lens;

    e->cpubuff = cpubuff;
    e->last_op = last_op;

    BPS_LOG(TRACE) << "EnqueueTensorBroadcast: " << e->tensor_name
                   << ", key=" << e->key
                   << ", size=" << e->output->size()
                   << ", device=" << device
                   << ", last_op=" << last_op;
    BytePSGlobal::GetScheduledQueue(BROADCAST)->addTask(e);
    return Status::OK();
}

Status InitTensor(std::shared_ptr<OpContext> context,
                std::shared_ptr<Tensor> tensor,
                std::shared_ptr<ReadyEvent> ready_event,
                const std::string &name, const int device,
                StatusCallback callback) {

    // Only rank 0 pushes the initialization
    if (BytePSGlobal::GetRank() == 0) {
        auto& bps_cxt = BytePSGlobal::GetContextFromName(name);
        ps::Key key = bps_cxt.key;
        BPS_LOG(TRACE) << "Init " << name
                       << ", key=" << key
                       << ", size=" << tensor->size()
                       << ", device=" << device;
        // get metadata
        size_t size = tensor->size();
        const int dtype = tensor->dtype();

        char* data;
        if (device != CPU_DEVICE_ID) { // GPU
            BPS_CHECK_EQ(size, bps_cxt.buff_len);
            BPS_CHECK(bps_cxt.cpubuff);
            CUDA_CALL(cudaMemcpy(bps_cxt.cpubuff, tensor->data(), size, cudaMemcpyDeviceToHost));
            data = const_cast<char*> (static_cast<const char*> (bps_cxt.cpubuff));
        } else { // CPU
            data = const_cast<char*> (static_cast<const char*> (tensor->data()));
        }

        // false means not to delete data when SArray is deleted
        ps::SArray<char> vals(data, size, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);

        auto& pskv = BytePSGlobal::EncodeDefaultKey(key, size);
        BytePSGlobal::GetPS()->Wait(BytePSGlobal::GetPS()->ZPush(
            pskv.keys, vals, pskv.lens, cmd));
    }

    ps::Postoffice::Get()->Barrier(0, ps::kWorkerGroup);

    callback(Status::OK());
    return Status::OK();
}

} // namespace common
} // namespace byteps
