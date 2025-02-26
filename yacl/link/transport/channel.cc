// Copyright 2019 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yacl/link/transport/channel.h"

#include <memory>
#include <set>

#include "absl/strings/numbers.h"
#include "spdlog/spdlog.h"

#include "yacl/base/buffer.h" 
#include "yacl/base/byte_container_view.h"
#include "yacl/base/exception.h"
#include "absl/strings/escaping.h"

namespace yacl::link::transport {

namespace {
// use acsii control code inside ack/fin msg key.
// avoid conflict to normal msg key.
const std::string kAckKey{'A', 'C', 'K', '\x01', '\x02'};
const std::string kFinKey{'F', 'I', 'N', '\x01', '\x02'};
const std::string kSeqKey{'\x01', '\x02'};

void NormalMessageKeyEnforce(std::string_view k) {
  YACL_ENFORCE(!k.empty(), "do not use empty key");
  YACL_ENFORCE(k.find(kSeqKey) == k.npos,
               "For developer: pls use another key for normal message.");
}

template <class View>
size_t ViewToSizeT(View v) {
  size_t ret = 0;
  YACL_ENFORCE(absl::SimpleAtoi(
      absl::string_view(reinterpret_cast<const char*>(v.data()), v.size()),
      &ret));
  return ret;
}

std::string BuildChannelKey(std::string_view msg_key, size_t seq_id) {
  return std::string(msg_key) + kSeqKey + std::to_string(seq_id);
}

std::pair<std::string, size_t> SplitChannelKey(std::string_view key) {
  auto pos = key.find(kSeqKey);

  std::pair<std::string, size_t> ret;
  ret.first = key.substr(0, pos);
  ret.second = ViewToSizeT(key.substr(pos + kSeqKey.size()));

  return ret;
}

}  // namespace

class SendTask {
 public:
  std::shared_ptr<Channel> channel_;
  Channel::Message msg_;
  const bool exit_if_async_error_;

  SendTask(std::shared_ptr<Channel> channel, Channel::Message msg,
           bool exit_if_async_error)
      : channel_(std::move(channel)),
        msg_(std::move(msg)),
        exit_if_async_error_(exit_if_async_error) {
    channel_->send_sync_.SendTaskStartNotify();
  }

  ~SendTask() {
    try {
      channel_->send_sync_.SendTaskFinishedNotify(msg_.seq_id_);
    } catch (...) {
      SPDLOG_ERROR("SendTaskFinishedNotify error");
      if (exit_if_async_error_) {
        exit(-1);
      }
    }
  }

  static void* Proc(void* args) {
    // take ownership of task.
    std::unique_ptr<SendTask> task(static_cast<SendTask*>(args));
    try {
      task->channel_->SendImpl(task->msg_.msg_key_, task->msg_.value_);
    } catch (const yacl::Exception& e) {
      SPDLOG_ERROR("SendImpl error {}", e.what());
      if (task->exit_if_async_error_) {
        exit(-1);
      }
    }

    return nullptr;
  }
};

void Channel::StartSendThread() {
  send_thread_ = std::thread([&]() {
    try {
      SendThread();
    } catch (const yacl::Exception& e) {
      SPDLOG_ERROR("SendThread error {}", e.what());
      if (exit_if_async_error_) {
        exit(-1);
      }
    }
  });
}

void Channel::SubmitSendTask(Message&& msg) {
  auto btask = std::make_unique<SendTask>(this->shared_from_this(),
                                          std::move(msg), exit_if_async_error_);
  bthread_t tid;
  if (bthread_start_background(&tid, nullptr, SendTask::Proc, btask.get()) ==
      0) {
    // bthread takes the ownership, release it.
    static_cast<void>(btask.release());
  } else {
    YACL_THROW("failed to push async sending job to bthread");
  }
}

std::optional<Channel::Message> Channel::MessageQueue::Pop(bool block) {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  if (block && queue_.empty()) {
    cond_.wait(lock);
  }

  if (!queue_.empty()) {
    auto msg = std::move(queue_.front());
    queue_.pop();
    return msg;
  } else {
    return {};
  }
}

void Channel::SendThread() {
  while (!send_thread_stoped_.load()) {
    auto msg = msg_queue_.Pop(true);
    if (!msg.has_value()) {
      continue;
    }
    auto seq_id = msg.value().seq_id_;
    SubmitSendTask(std::move(msg.value()));
    ThrottleWindowWait(seq_id);
  }

  // link is closing, send all pending msgs
  while (true) {
    auto msg = msg_queue_.Pop(false);
    if (!msg.has_value()) {
      break;
    }
    SubmitSendTask(std::move(msg.value()));
  }
}

class SendChunkedWindow
    : public std::enable_shared_from_this<SendChunkedWindow> {
 public:
  explicit SendChunkedWindow(int64_t limit) : parallel_limit_(limit) {
    YACL_ENFORCE(parallel_limit_ > 0);
  }

  void OnPushDone(std::optional<std::exception> e) noexcept {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    running_push_--;

    if (e.has_value()) {
      async_exception_ = std::move(e);
    }
    cond_.notify_all();
  }

  void Finished() {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    while (running_push_ != 0) {
      cond_.wait(lock);
      if (async_exception_.has_value()) {
        throw async_exception_.value();
      }
    }
  }

  class Token {
   public:
    explicit Token(std::shared_ptr<SendChunkedWindow> window)
        : window_(std::move(window)) {}

    void SetException(std::optional<std::exception>& e) {
      if (!e.has_value()) {
        return;
      }
      exception_ = std::move(e);
    }

    ~Token() { window_->OnPushDone(exception_); }

   private:
    std::shared_ptr<SendChunkedWindow> window_;
    std::optional<std::exception> exception_;
  };

  std::unique_ptr<Token> GetToken() {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    running_push_++;

    while (running_push_ >= parallel_limit_) {
      cond_.wait(lock);
      if (async_exception_.has_value()) {
        throw async_exception_.value();
      }
    }

    return std::make_unique<Token>(this->shared_from_this());
  }

 private:
  const int64_t parallel_limit_;
  int64_t running_push_ = 0;
  bthread::Mutex mutex_;
  bthread::ConditionVariable cond_;
  std::optional<std::exception> async_exception_;
};

class SendChunkedTask {
 public:
  SendChunkedTask(std::shared_ptr<TransportLink> delegate,
                  std::unique_ptr<SendChunkedWindow::Token> token,
                  std::unique_ptr<::google::protobuf::Message> request)
      : link_(std::move(delegate)),
        token_(std::move(token)),
        request_(std::move(request)) {
    YACL_ENFORCE(request_, "request is null");
    YACL_ENFORCE(token_, "token is null");
    YACL_ENFORCE(link_, "channel is null");
  }

  static void* Proc(void* param) {
    std::unique_ptr<SendChunkedTask> task(static_cast<SendChunkedTask*>(param));
    std::optional<std::exception> except;
    try {
      task->link_->SendRequest(*(task->request_), 0);
    } catch (Exception& e) {
      except = e;
      task->token_->SetException(except);
    }

    return nullptr;
  };

 private:
  std::shared_ptr<TransportLink> link_;
  std::unique_ptr<SendChunkedWindow::Token> token_;
  std::unique_ptr<::google::protobuf::Message> request_;
};

void Channel::SendChunked(const std::string& key, ByteContainerView value) {
  // SPDLOG_INFO("[SendChunked] 收到 FIN 结束消息 ，消息传递已完成");
  const size_t bytes_per_chunk = link_->GetMaxBytesPerChunk();
  const size_t num_bytes = value.size();
  const size_t num_chunks = (num_bytes + bytes_per_chunk - 1) / bytes_per_chunk;

  constexpr uint32_t kParallelSize = 8;
  auto window = std::make_shared<SendChunkedWindow>(kParallelSize);

  for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
    const size_t chunk_offset = chunk_idx * bytes_per_chunk;

    auto request = link_->PackChunkedRequest(
        key,
        ByteContainerView(
            value.data() + chunk_offset,
            std::min(bytes_per_chunk, value.size() - chunk_offset)),
        chunk_offset, num_bytes);

    auto task = std::make_unique<SendChunkedTask>(link_, window->GetToken(),
                                                  std::move(request));
    bthread_t tid;
    if (bthread_start_background(&tid, nullptr, SendChunkedTask::Proc,
                                 task.get()) == 0) {
      (void)task.release();
    } else {
      YACL_THROW("Start bthread error for Chunk (key: {}, {} of {}) error", key,
                 chunk_idx, num_chunks);
    }
  }
  window->Finished();
}

void Channel::SendTaskSynchronizer::SendTaskStartNotify() {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  running_tasks_++;
}

void Channel::SendTaskSynchronizer::SendTaskFinishedNotify(size_t seq_id) {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  running_tasks_--;
  if (seq_id != 0) {
    finished_ids_.Insert(seq_id);
  }
  finished_cond_.notify_all();
}

void Channel::SendTaskSynchronizer::WaitSeqIdSendFinished(size_t seq_id) {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  while (!finished_ids_.Contains(seq_id)) {
    finished_cond_.wait(lock);
  }
}

void Channel::SendTaskSynchronizer::WaitAllSendFinished() {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  while (running_tasks_ > 0) {
    finished_cond_.wait(lock);
  }
}

Buffer Channel::Recv(const std::string& msg_key) {
  NormalMessageKeyEnforce(msg_key);
  Buffer value;
  size_t seq_id = 0;
  {
    std::unique_lock<bthread::Mutex> lock(msg_mutex_);
    auto stop_waiting = [&] {
      auto itr = this->msg_db_.find(msg_key);
      if (itr == this->msg_db_.end()) {
        return false;
      } else {
        std::tie(value, seq_id) = std::move(itr->second);
        this->msg_db_.erase(itr);
        return true;
      }
    };
    while (!stop_waiting()) {
      //                                timeout_us
      if (msg_db_cond_.wait_for(lock, static_cast<int64_t>(recv_timeout_ms_) *
                                          1000) == ETIMEDOUT) {
        YACL_THROW_IO_ERROR("Get data timeout, key={}", msg_key);
      }
    }
  }
  SendAck(seq_id);

  return value;
}

void Channel::SendAck(size_t seq_id) {
  if (seq_id > 0) {
    // 0 seq id use for TestSend/TestRecv, no need to send ack.
    SubmitSendTask(Message(0, kAckKey, Buffer(std::to_string(seq_id))));
  }
}

template <typename T>
void Channel::OnNormalMessage(const std::string& key, T&& v) {
  std::string msg_key;
  size_t seq_id = 0;
  std::tie(msg_key, seq_id) = SplitChannelKey(key);

  if (seq_id > 0 && !received_msg_ids_.Insert(seq_id)) {
    // 0 seq id use for TestSend/TestRecv, skip duplicate test.
    // Duplicate seq id found. may be caused by rpc retry, ignore
    SPDLOG_WARN("Duplicate seq_id found, key {} seq_id {}", msg_key, seq_id);
    return;
  }

  if (!waiting_finish_.load()) {
    auto pair =
        msg_db_.emplace(msg_key, std::make_pair(std::forward<T>(v), seq_id));
    if (seq_id > 0 && !pair.second) {
      YACL_THROW(
          "For developer: BUG! PLS do not use same key for multiple msg, "
          "Duplicate key {} with new seq_id {}, old seq_id {}.",
          msg_key, seq_id, pair.first->second.second);
    }
  } else {
    SendAck(seq_id);
    SPDLOG_WARN("Asymmetric logic exist, auto ack key {} seq_id {}", msg_key,
                seq_id);
  }
  msg_db_cond_.notify_all();
}

class ChunkedMessage {
 public:
  explicit ChunkedMessage(int64_t message_length) : message_(message_length) {}

  void AddChunk(int64_t offset, ByteContainerView data) {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    if (received_.emplace(offset).second) {
      std::memcpy(message_.data<std::byte>() + offset, data.data(),
                  data.size());
      bytes_written_ += data.size();
    }
  }

  bool IsFullyFilled() {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    return bytes_written_ == message_.size();
  }

  Buffer&& Reassemble() {
    std::unique_lock<bthread::Mutex> lock(mutex_);
    return std::move(message_);
  }

 protected:
  bthread::Mutex mutex_;
  std::set<int64_t> received_;
  // chunk index to value.
  int64_t bytes_written_{0};
  Buffer message_;
};

void Channel::OnChunkedMessage(const std::string& key, ByteContainerView value,
                               size_t offset, size_t total_length) {
  if (offset + value.size() > total_length) {
    YACL_THROW_LOGIC_ERROR(
        "invalid chunk info, offset={}, chun size = {}, total_length={}",
        offset, value.size(), total_length);
  }

  bool should_reassemble = false;
  std::shared_ptr<ChunkedMessage> data;
  {
    std::unique_lock<bthread::Mutex> lock(chunked_values_mutex_);
    auto itr = chunked_values_.find(key);
    if (itr == chunked_values_.end()) {
      itr = chunked_values_
                .emplace(key, std::make_shared<ChunkedMessage>(total_length))
                .first;
    }

    data = itr->second;
    data->AddChunk(offset, value);

    if (data->IsFullyFilled()) {
      chunked_values_.erase(itr);

      // only one thread do the reassemble
      should_reassemble = true;
    }
  }

  if (should_reassemble) {
    OnMessage(key, data->Reassemble());
  }
}

void Channel::OnMessage(const std::string& key, ByteContainerView value) {
  std::unique_lock<bthread::Mutex> lock(msg_mutex_);
  if (key == kAckKey) {
    size_t seq_id = ViewToSizeT(value);
    SPDLOG_INFO("[OnMessage] 收到 ACK 确认消息，当前 seq_id:{}", seq_id);
    if (received_ack_ids_.Insert(seq_id)) {
      ack_fin_cond_.notify_all();
    } else {
      SPDLOG_WARN("Duplicate ACK id {}", seq_id);
    }
  } else if (key == kFinKey) {
    SPDLOG_INFO("[OnMessage] 收到 FIN 结束消息 ，消息传递已完成");
    if (!received_fin_) {
      received_fin_ = true;
      peer_sent_msg_count_ = ViewToSizeT(value);
      SPDLOG_INFO("[OnMessage] 对方发送消息数为:{}", peer_sent_msg_count_);
      ack_fin_cond_.notify_all();
    } else {
      SPDLOG_WARN("Duplicate FIN");
    }
  } else {
    SPDLOG_INFO("[OnMessage] 收到普通消息, key:{}", key);
    if(value.size()>16){
    absl::string_view original_str_view = static_cast<absl::string_view>(Buffer(value));
    auto len = original_str_view.size();
    auto display_str_view_pre = original_str_view.substr(0, 8);
    auto display_str_view_suf = original_str_view.substr(len-8, len);
    SPDLOG_INFO("[OnMessage] 收到普通消息 value: {}", absl::BytesToHexString(display_str_view_pre)+"..."+absl::BytesToHexString(display_str_view_suf));
  }
  else{
    SPDLOG_INFO("[OnMessage] 收到普通消息 value: {}", absl::BytesToHexString(static_cast<absl::string_view>(value)));
  }
    OnNormalMessage(key, value);
  }
}

void Channel::SetRecvTimeout(uint64_t recv_timeout_ms) {
  recv_timeout_ms_ = recv_timeout_ms;
}

uint64_t Channel::GetRecvTimeout() const { return recv_timeout_ms_; }

void Channel::SendAsync(const std::string& msg_key, ByteContainerView value) {
  SendAsync(msg_key, Buffer(value));
}

void Channel::MessageQueue::Push(Message&& msg) {
  std::unique_lock<bthread::Mutex> lock(mutex_);
  SPDLOG_INFO("[Push] 信息进入消息队列：seq_id:{}", msg.seq_id_);
  SPDLOG_INFO("[Push] 信息进入消息队列：msg_key:{}", msg.msg_key_);
  if(msg.value_.size()>16){
    absl::string_view original_str_view = static_cast<absl::string_view>(msg.value_);
    auto len = original_str_view.size();
    auto display_str_view_pre = original_str_view.substr(0, 8);
    auto display_str_view_suf = original_str_view.substr(len-8, len);
    SPDLOG_INFO("[Push] 信息进入消息队列 value: {}", absl::BytesToHexString(display_str_view_pre)+"..."+absl::BytesToHexString(display_str_view_suf));
  }
  else{
    SPDLOG_INFO("[Push] 信息进入消息队列 value: {}", absl::BytesToHexString(static_cast<absl::string_view>(msg.value_)));
  }
  queue_.push(std::move(msg));
  cond_.notify_all();
}

void Channel::SendAsync(const std::string& msg_key, Buffer&& value) {
  YACL_ENFORCE(!waiting_finish_.load(),
               "SendAsync is not allowed when channel is closing");

  NormalMessageKeyEnforce(msg_key);
  size_t seq_id = msg_seq_id_.fetch_add(1) + 1;
  auto key = BuildChannelKey(msg_key, seq_id);
  SPDLOG_INFO("[SendAsync] 异步发送消息 seq_id: {}", seq_id);
  SPDLOG_INFO("[SendAsync] 异步发送消息密钥 msg_key: {}", msg_key);
  SPDLOG_INFO("[SendAsync] 异步发送密钥 key: {}", key);
  if(value.size()>16){
    absl::string_view original_str_view = static_cast<absl::string_view>(value);
    auto len = original_str_view.size();
    auto display_str_view_pre = original_str_view.substr(0, 8);
    auto display_str_view_suf = original_str_view.substr(len-8, len);
    SPDLOG_INFO("[SendAsync] 异步发送消息内容 value: {}", absl::BytesToHexString(display_str_view_pre)+"..."+absl::BytesToHexString(display_str_view_suf));
  }
  else{
    SPDLOG_INFO("[SendAsync] 异步发送消息内容 value: {}", absl::BytesToHexString(static_cast<absl::string_view>(value)));
  }
  msg_queue_.Push(Message(seq_id, std::move(key), std::move(value)));
}

void Channel::Send(const std::string& msg_key, ByteContainerView value) {
  YACL_ENFORCE(!waiting_finish_.load(),
               "Send is not allowed when channel is closing");
  NormalMessageKeyEnforce(msg_key);
  size_t seq_id = msg_seq_id_.fetch_add(1) + 1;
  auto key = BuildChannelKey(msg_key, seq_id);
  SPDLOG_INFO("[Send] 发送消息 seq_id: {}", seq_id);
  SPDLOG_INFO("[Send] 发送消息密钥 msg_key: {}", msg_key);
  SPDLOG_INFO("[Send] 发送密钥 key: {}", key);
  
  if(value.size()>16){
    absl::string_view original_str_view = static_cast<absl::string_view>(Buffer(value));
    auto len = original_str_view.size();
    auto display_str_view_pre = original_str_view.substr(0, 8);
    auto display_str_view_suf = original_str_view.substr(len-8, len);
    SPDLOG_INFO("[Send] 发送消息内容 value: {}", absl::BytesToHexString(display_str_view_pre)+"..."+absl::BytesToHexString(display_str_view_suf));
  }
  else{
    SPDLOG_INFO("[Send] 发送消息内容 value: {}", absl::BytesToHexString(static_cast<absl::string_view>(Buffer(value))));
  }

  msg_queue_.Push(Message(seq_id, std::move(key), value));
  send_sync_.WaitSeqIdSendFinished(seq_id);
}

void Channel::SendAsyncThrottled(const std::string& msg_key,
                                 ByteContainerView value) {
  SendAsyncThrottled(msg_key, Buffer(value));
}

void Channel::SendAsyncThrottled(const std::string& msg_key, Buffer&& value) {
  YACL_ENFORCE(!waiting_finish_.load(),
               "SendAsync is not allowed when channel is closing");
  NormalMessageKeyEnforce(msg_key);
  size_t seq_id = msg_seq_id_.fetch_add(1) + 1;
  auto key = BuildChannelKey(msg_key, seq_id);
  SPDLOG_INFO("[SendAsyncThrottled] 限制型异步发送消息 seq_id: {}", seq_id);
  SPDLOG_INFO("[SendAsyncThrottled] 限制型异步发送消息密钥 msg_key: {}", msg_key);
  SPDLOG_INFO("[SendAsyncThrottled] 限制型异步发送密钥 key: {}", key);
  if(value.size()>16){
    absl::string_view original_str_view = static_cast<absl::string_view>(value);
    auto len = original_str_view.size();
    auto display_str_view_pre = original_str_view.substr(0, 8);
    auto display_str_view_suf = original_str_view.substr(len-8, len);
    SPDLOG_INFO("[SendAsyncThrottled] 限制型异步发送消息内容 value: {}", absl::BytesToHexString(display_str_view_pre)+"..."+absl::BytesToHexString(display_str_view_suf));
  }
  else{
    SPDLOG_INFO("[SendAsyncThrottled] 限制型异步发送消息内容 value: {}", absl::BytesToHexString(static_cast<absl::string_view>(value)));
  }
  msg_queue_.Push(Message(seq_id, std::move(key), std::move(value)));
  ThrottleWindowWait(seq_id);
}

void Channel::TestSend(uint32_t timeout) {
  YACL_ENFORCE(!waiting_finish_.load(),
               "TestSend is not allowed when channel is closing");
  const auto msg_key = fmt::format("connect_{}", link_->LocalRank());
  const auto key = BuildChannelKey(msg_key, 0);
  SendImpl(key, "", timeout);
}

void Channel::TestRecv() {
  const auto msg_key = fmt::format("connect_{}", link_->RemoteRank());
  Recv(msg_key);
}

// all sender thread wait on it's send order.
void Channel::ThrottleWindowWait(size_t wait_count) {
  if (throttle_window_size_ == 0) {
    return;
  }
  std::unique_lock<bthread::Mutex> lock(msg_mutex_);
  while ((throttle_window_size_ != 0) &&
         (received_ack_ids_.Count() + throttle_window_size_ <= wait_count)) {
    //                               timeout_us
    if (ack_fin_cond_.wait_for(
            lock, static_cast<int64_t>(recv_timeout_ms_) * 1000) == ETIMEDOUT) {
      YACL_THROW_IO_ERROR("Throttle window wait timeout");
    }
  }
}

void Channel::WaitAsyncSendToFinish() {
  send_thread_stoped_.store(true);
  msg_queue_.EmptyNotify();
  send_thread_.join();
  send_sync_.WaitAllSendFinished();
}

void Channel::MessageQueue::EmptyNotify() { cond_.notify_all(); }

void Channel::WaitForFinAndFlyingMsg() {
  size_t sent_msg_count = msg_seq_id_;
  SubmitSendTask(Message(0, kFinKey, Buffer(std::to_string(sent_msg_count))));
  {
    std::unique_lock<bthread::Mutex> lock(msg_mutex_);
    while (!received_fin_) {
      ack_fin_cond_.wait(lock);
    }
  }
  {
    std::unique_lock<bthread::Mutex> lock(msg_mutex_);
    if (peer_sent_msg_count_ == 0) {
      // peer send no thing, no need waiting.
      return;
    }
    // wait until recv all msg from 1 to peer_sent_msg_count_
    while (received_msg_ids_.SegmentsCount() > 1 ||
           !received_msg_ids_.Contains(1) ||
           !received_msg_ids_.Contains(peer_sent_msg_count_)) {
      msg_db_cond_.wait(lock);
    }
  }
}

void Channel::StopReceivingAndAckUnreadMsgs() {
  std::unique_lock<bthread::Mutex> lock(msg_mutex_);
  waiting_finish_.store(true);
  for (auto& msg : msg_db_) {
    auto seq_id = msg.second.second;
    SPDLOG_WARN("Asymmetric logic exist, clear unread key {}, seq_id {}",
                msg.first, seq_id);
    SendAck(seq_id);
  }
  msg_db_.clear();
}

void Channel::WaitForFlyingAck() {
  size_t sent_msg_count = msg_seq_id_;
  std::unique_lock<bthread::Mutex> lock(msg_mutex_);
  if (sent_msg_count == 0) {
    // send no thing, no need waiting.
    return;
  }

  // wait until recv all ack from 1 to sent_msg_count
  while (received_ack_ids_.SegmentsCount() > 1 ||
         !received_ack_ids_.Contains(1) ||
         !received_ack_ids_.Contains(sent_msg_count)) {
    ack_fin_cond_.wait(lock);
  }
}

void Channel::WaitLinkTaskFinish() {
  // 4 steps to total stop link.
  // send ack for msg exist in msg_db_ that unread by up layer logic.
  // stop OnMessage & auto ack all normal msg from now on.
  StopReceivingAndAckUnreadMsgs();
  // wait for last fin msg contain peer's send msg count.
  // then check if received count is equal to peer's send count.
  // we can not close server port if peer still sending msg
  // or peer's gateway will throw 504 error.
  WaitForFinAndFlyingMsg();
  // make sure all Async send is finished.
  WaitAsyncSendToFinish();
  // at least, wait for all ack msg.
  WaitForFlyingAck();
  // after all, we can safely close server port and exit.
}

void Channel::OnRequest(const ::google::protobuf::Message& request,
                        ::google::protobuf::Message* response) {
  YACL_ENFORCE(response != nullptr, "response should not be null");
  YACL_ENFORCE(link_ != nullptr, "delegate should not be null");
  
  SPDLOG_INFO("[OnRequest] 处理 Request");

  link_->FillResponseOk(request, response);

  if (link_->IsMonoRequest(request)) {
    SPDLOG_INFO("[OnRequest] 该 Request 为 MonoRequest");
    std::string key;
    ByteContainerView value;
    link_->UnpackMonoRequest(request, &key, &value);
    SPDLOG_INFO("{} recv {}", link_->LocalRank(), key);
    SPDLOG_DEBUG("{} recv {}", link_->LocalRank(), key);
    OnMessage(key, value);
  } else if (link_->IsChunkedRequest(request)) {
    SPDLOG_INFO("[OnRequest] 该 Request 为 ChunkedRequest");
    std::string key;
    ByteContainerView value;
    size_t offset = 0;
    size_t total_length = 0;
    SPDLOG_INFO("{} recv {}", link_->LocalRank(), key);
    SPDLOG_DEBUG("{} recv {}", link_->LocalRank(), key);
    link_->UnpackChunckRequest(request, &key, &value, &offset, &total_length);
    OnChunkedMessage(key, value, offset, total_length);
  } else {
    link_->FillResponseError(request, response);
  }
}

}  // namespace yacl::link::transport
