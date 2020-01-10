////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_GENERAL_SERVER_H2_COMM_TASK_H
#define ARANGOD_GENERAL_SERVER_H2_COMM_TASK_H 1

#include "GeneralServer/AsioSocket.h"
#include "GeneralServer/GeneralCommTask.h"

#include <nghttp2/nghttp2.h>
#include <velocypack/StringRef.h>
#include <boost/lockfree/queue.hpp>
#include <memory>

namespace arangodb {
class HttpRequest;
class HttpResponse;

namespace basics {
class StringBuffer;
}

namespace rest {

template <SocketType T>
class H2CommTask final : public GeneralCommTask<T> {
 public:
  H2CommTask(GeneralServer& server, ConnectionInfo, std::unique_ptr<AsioSocket<T>> so);
  ~H2CommTask() noexcept;

  void start() override;
  /// @brief upgrade from  H1 connection, must not call start
  void upgrade(std::unique_ptr<HttpRequest> req);

 protected:
  bool readCallback(asio_ns::error_code ec) override;

  void sendResponse(std::unique_ptr<GeneralResponse> response, RequestStatistics* stat) override;

  std::unique_ptr<GeneralResponse> createResponse(rest::ResponseCode, uint64_t messageId) override;

 private:
  static int on_begin_headers(nghttp2_session* session,
                              const nghttp2_frame* frame, void* user_data);
  static int on_header(nghttp2_session* session, const nghttp2_frame* frame,
                       const uint8_t* name, size_t namelen, const uint8_t* value,
                       size_t valuelen, uint8_t flags, void* user_data);
  static int on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame,
                           void* user_data);
  static int on_data_chunk_recv(nghttp2_session* session, uint8_t flags, int32_t stream_id,
                                const uint8_t* data, size_t len, void* user_data);
  static int on_stream_close(nghttp2_session* session, int32_t stream_id,
                             uint32_t error_code, void* user_data);
  static int on_frame_send(nghttp2_session* session, const nghttp2_frame* frame,
                           void* user_data);

  static int on_frame_not_send(nghttp2_session* session, const nghttp2_frame* frame,
                               int lib_error_code, void* user_data);

 private:
  // ongoing Http2 stream
  struct Stream {
    Stream(std::unique_ptr<HttpRequest> req) : request(std::move(req)) {}

    std::string origin;
    std::unique_ptr<HttpRequest> request;
    std::unique_ptr<HttpResponse> response;

    size_t responseOffset = 0;
  };

  /// init h2 session
  void initNgHttp2Session();

  void processStream(Stream* strm);

  /// should close connection
  bool shouldStop() const;

  // may be used to signal a write from sendResponse
  void signalWrite();

  // queue the response onto the session, call only on IO thread
  void queueHttp2Responses();

  /// actually perform writing
  void doWrite();

  Stream* createStream(int32_t sid, std::unique_ptr<HttpRequest>);
  Stream* findStream(int32_t sid) const;

 private:
  static constexpr size_t kOutBufferLen = 32 * 1024 * 1024;
  std::array<uint8_t, kOutBufferLen> _outbuffer;

  boost::lockfree::queue<uint32_t, boost::lockfree::capacity<512>> _waitingResponses;

  std::map<int32_t, std::unique_ptr<Stream>> _streams;

  nghttp2_session* _session = nullptr;
  bool _writing = false;

  std::atomic<bool> _signaledWrite = false;
};
}  // namespace rest
}  // namespace arangodb

#endif
