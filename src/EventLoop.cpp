#include "lift/EventLoop.h"

#include <curl/multi.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace lift {

template <typename O, typename I>
static auto uv_type_cast(I* i) -> O*
{
    auto* void_ptr = static_cast<void*>(i);
    return static_cast<O*>(void_ptr);
}

class CurlContext {
public:
    explicit CurlContext(
        EventLoop& event_loop)
        : m_event_loop(event_loop)
    {
        m_poll_handle.data = this;
    }

    ~CurlContext() = default;

    CurlContext(const CurlContext&) = delete;
    CurlContext(CurlContext&&) = delete;
    auto operator=(const CurlContext&) = delete;
    auto operator=(CurlContext&&) = delete;

    auto Init(
        uv_loop_t* uv_loop,
        curl_socket_t sock_fd) -> void
    {
        m_sock_fd = sock_fd;
        uv_poll_init(uv_loop, &m_poll_handle, m_sock_fd);
    }

    auto Close()
    {
        uv_poll_stop(&m_poll_handle);
        /**
         * uv requires us to jump through a few hoops before we can delete ourselves.
         */
        uv_close(uv_type_cast<uv_handle_t>(&m_poll_handle), CurlContext::on_close);
    }

    inline auto GetEventLoop() -> EventLoop& { return m_event_loop; }
    inline auto GetUvPollHandle() -> uv_poll_t& { return m_poll_handle; }
    inline auto GetCurlSockFd() -> curl_socket_t { return m_sock_fd; }

    static auto on_close(
        uv_handle_t* handle) -> void
    {
        auto* curl_context = static_cast<CurlContext*>(handle->data);
        /**
         * uv has signaled that it is finished with the m_poll_handle,
         * we can now safely tell the event loop to re-use this curl context.
         */
        curl_context->m_event_loop.m_curl_context_ready.emplace_back(curl_context);
    }

private:
    EventLoop& m_event_loop;
    uv_poll_t m_poll_handle {};
    curl_socket_t m_sock_fd { CURL_SOCKET_BAD };
};

auto curl_start_timeout(
    CURLM* cmh,
    long timeout_ms,
    void* user_data) -> void;

auto curl_handle_socket_actions(
    CURL* curl,
    curl_socket_t socket,
    int action,
    void* user_data,
    void* socketp) -> int;

auto uv_close_callback(
    uv_handle_t* handle) -> void;

auto on_uv_timeout_callback(
    uv_timer_t* handle) -> void;

auto on_uv_curl_perform_callback(
    uv_poll_t* req,
    int status,
    int events) -> void;

auto requests_accept_async(uv_async_t* handle) -> void;

auto on_response_wait_time_expired_callback(uv_timer_t* handle) -> void;


EventLoop::EventLoop()
{
    uv_async_init(m_loop, &m_async, requests_accept_async);
    m_async.data = this;

    uv_timer_init(m_loop, &m_timeout_timer);
    m_timeout_timer.data = this;

    // Initialize the request timer, which will be used to "stop" requests that have not received
    // a response with in the requested time.
    uv_timer_init(m_loop, &m_response_timer);
    m_response_timer.data = this;

    curl_multi_setopt(m_cmh, CURLMOPT_SOCKETFUNCTION, curl_handle_socket_actions);
    curl_multi_setopt(m_cmh, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(m_cmh, CURLMOPT_TIMERFUNCTION, curl_start_timeout);
    curl_multi_setopt(m_cmh, CURLMOPT_TIMERDATA, this);

    m_background_thread = std::thread { [this] { run(); } };

    /**
     * Wait for the thread to spin-up and run the event loop,
     * this means when the constructor returns the user can start adding requests
     * immediately without waiting.
     */
    while (!IsRunning()) {
        std::this_thread::sleep_for(1ms);
    }
}

EventLoop::~EventLoop()
{
    m_is_stopping = true;

    while (HasUnfinishedRequests()) {
        std::this_thread::sleep_for(1ms);
    }

    uv_timer_stop(&m_timeout_timer);
    uv_close(uv_type_cast<uv_handle_t>(&m_timeout_timer), uv_close_callback);
    uv_timer_stop(&m_response_timer);
    uv_close(uv_type_cast<uv_handle_t>(&m_response_timer), uv_close_callback);
    uv_close(uv_type_cast<uv_handle_t>(&m_async), uv_close_callback);
    uv_async_send(&m_async); // fake a request to make sure the loop wakes up
    uv_stop(m_loop);

    while (!m_timeout_timer_closed && !m_request_timer_closed && !m_async_closed) {
        std::this_thread::sleep_for(1ms);
    }

    m_background_thread.join();

    curl_multi_cleanup(m_cmh);
    uv_loop_close(m_loop);
}

auto EventLoop::IsRunning() -> bool
{
    return m_is_running;
}

auto EventLoop::Stop() -> void
{
    m_is_stopping = true;
}

static auto has_pending_requests(std::mutex& mutex, const std::vector<RequestHandle>& pending_requests) -> bool
{
    std::unique_lock lock{mutex};
    return !pending_requests.empty();
}

auto EventLoop::HasUnfinishedRequests() -> bool
{
    return (
           (m_active_request_count.load() > 0)
        || has_pending_requests(m_pending_requests_lock, m_pending_requests)
    );
}

auto EventLoop::GetRequestPool() -> RequestPool&
{
    return m_request_pool;
}

auto EventLoop::StartRequest(
    RequestHandle request) -> bool
{
    if (m_is_stopping) {
        return false;
    }
    // We'll prepare now since it won't block the event loop thread.
    request->prepareForPerform();
    {
        std::lock_guard<std::mutex> guard { m_pending_requests_lock };
        m_pending_requests.emplace_back(std::move(request));
    }
    uv_async_send(&m_async);
    return true;
}

auto EventLoop::stopTimedOutRequests() -> void
{
    // If the wrappers multiset is empty, then we don't have anything to stop
    if (m_response_wait_time_wrappers.empty())
    {
        return;
    }

    // Get the current timepoint from libuv (it caches the current timepoint at the beginning
    // of every cycle through the event loop, so this should not be expensive).
    auto now = uv_now(m_loop);

    for (
        auto current_wrapper = m_response_wait_time_wrappers.begin();
        current_wrapper != m_response_wait_time_wrappers.end() && current_wrapper->GetData().m_timeout_time <= now;
        // We increment current_wrapper by setting it to the iterator returned from erasing the current one
    )
    {
        // Get the shared pointer out of the ResponseWaitTimeWrapper and into a unique pointer so it cleans
        // itself up at the end of this method.
        auto& data = current_wrapper->GetData();
        auto shared_request = std::unique_ptr<std::shared_ptr<SharedRequest>>(data.m_shared_request_ptr_pointer);
        
        // Call onComplete on the underlying Request object, copying in the shared_pointer
        // so we get a correct reference count.
        (**shared_request).GetAsReference().onComplete(*this, *shared_request, true);
        
        // Update current_wrapper with the result of removing the wrapper from the multiset
        current_wrapper = removeTimeoutByIterator(current_wrapper);
    }
    
    // If there are still items in the multiset, get the first item and use its time to reset the request timer.
    if (!m_response_wait_time_wrappers.empty())
    {
        auto& request_time = m_response_wait_time_wrappers.begin()->GetData().m_timeout_time;
        uv_timer_stop(&m_response_timer);
    
        uv_timer_start(
            &m_response_timer,
            on_response_wait_time_expired_callback,
            request_time - now,
            0);
    }
}

auto EventLoop::removeTimeoutByIterator(std::multiset<ResponseWaitTimeWrapper>::iterator request_to_remove) -> std::multiset<ResponseWaitTimeWrapper>::iterator
{
    // Get the shared pointer to SharedRequest into a unique pointer so it cleans itself up.
    auto request_ptr = *request_to_remove->GetData().m_shared_request_ptr_pointer;
    auto shared_request = std::make_unique<const std::shared_ptr<SharedRequest>>(request_ptr);
    
    // Reset the iterator stored on the Request so it can't be reused.
    (*shared_request)->GetAsPointer()->m_response_wait_time_set_iterator.reset();
    
    return m_response_wait_time_wrappers.erase(request_to_remove);
}

auto EventLoop::run() -> void
{
    m_is_running = true;
    uv_run(m_loop, UV_RUN_DEFAULT);
    m_is_running = false;
}

auto EventLoop::checkActions() -> void
{
    checkActions(CURL_SOCKET_TIMEOUT, 0);
}

auto EventLoop::checkActions(
    curl_socket_t socket,
    int event_bitmask) -> void
{
    int running_handles = 0;
    CURLMcode curl_code = CURLM_OK;
    do {
        curl_code = curl_multi_socket_action(m_cmh, socket, event_bitmask, &running_handles);
    } while (curl_code == CURLM_CALL_MULTI_PERFORM);

    CURLMsg* message = nullptr;
    int msgs_left = 0;

    while ((message = curl_multi_info_read(m_cmh, &msgs_left)) != nullptr) {
        if (message->msg == CURLMSG_DONE) {
            /**
             * Per docs do not use 'message' after calling curl_multi_remove_handle.
             */
            CURL* easy_handle = message->easy_handle;
            CURLcode easy_result = message->data.result;

            // So the reacquired shared_ptr destructs itself correctly, let's get it into a unique pointer
            std::unique_ptr<std::shared_ptr<SharedRequest>> shared_request_ptr_pointer;
            curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &shared_request_ptr_pointer);
            curl_multi_remove_handle(m_cmh, easy_handle);

            auto shared_request_ptr = *shared_request_ptr_pointer;
            
            shared_request_ptr->GetAsReference().setCompletionStatus(easy_result);
            shared_request_ptr->GetAsReference().onComplete(*this, shared_request_ptr);

            --m_active_request_count;
        }
    }
}

auto curl_start_timeout(
    CURLM* /*cmh*/,
    long timeout_ms,
    void* user_data) -> void
{
    auto* event_loop = static_cast<EventLoop*>(user_data);

    // Stop the current timer regardless.
    uv_timer_stop(&event_loop->m_timeout_timer);

    if (timeout_ms > 0) {
        uv_timer_start(
            &event_loop->m_timeout_timer,
            on_uv_timeout_callback,
            static_cast<uint64_t>(timeout_ms),
            0);
    } else if (timeout_ms == 0) {
        event_loop->checkActions();
    }
}

auto curl_handle_socket_actions(
    CURL* /*curl*/,
    curl_socket_t socket,
    int action,
    void* user_data,
    void* socketp) -> int
{
    auto* event_loop = static_cast<EventLoop*>(user_data);

    CurlContext* curl_context = nullptr;
    if (action == CURL_POLL_IN || action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
        if (socketp != nullptr) {
            // existing request
            curl_context = static_cast<CurlContext*>(socketp);
        } else {
            // new request, and no curl context's available? make one
            if (event_loop->m_curl_context_ready.empty()) {
                auto curl_context_ptr = std::make_unique<CurlContext>(*event_loop);
                curl_context = curl_context_ptr.release();
            } else {
                curl_context = event_loop->m_curl_context_ready.front().release();
                event_loop->m_curl_context_ready.pop_front();
            }

            curl_context->Init(event_loop->m_loop, socket);
            curl_multi_assign(event_loop->m_cmh, socket, static_cast<void*>(curl_context));
        }
    }

    switch (action) {
    case CURL_POLL_IN:
        uv_poll_start(&curl_context->GetUvPollHandle(), UV_READABLE, on_uv_curl_perform_callback);
        break;
    case CURL_POLL_OUT:
        uv_poll_start(&curl_context->GetUvPollHandle(), UV_WRITABLE, on_uv_curl_perform_callback);
        break;
    case CURL_POLL_INOUT:
        uv_poll_start(&curl_context->GetUvPollHandle(), UV_READABLE | UV_WRITABLE, on_uv_curl_perform_callback);
        break;
    case CURL_POLL_REMOVE:
        if (socketp != nullptr) {
            curl_context = static_cast<CurlContext*>(socketp);
            curl_context->Close(); // signal this handle is done
            curl_multi_assign(event_loop->m_cmh, socket, nullptr);
        }
        break;
    default:
        break;
    }

    return 0;
}

auto uv_close_callback(uv_handle_t* handle) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);
    if (handle == uv_type_cast<uv_handle_t>(&event_loop->m_async))
    {
        event_loop->m_async_closed = true;
    }
    else if (handle == uv_type_cast<uv_handle_t>(&event_loop->m_timeout_timer))
    {
        event_loop->m_timeout_timer_closed = true;
    }
    else if (handle == uv_type_cast<uv_handle_t>(&event_loop->m_response_timer))
    {
        event_loop->m_request_timer_closed = true;
    }
}

auto on_uv_timeout_callback(
    uv_timer_t* handle) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);
    event_loop->checkActions();
}

auto on_uv_curl_perform_callback(
    uv_poll_t* req,
    int status,
    int events) -> void
{
    auto* curl_context = static_cast<CurlContext*>(req->data);
    auto& event_loop = curl_context->GetEventLoop();

    int32_t action = 0;
    if (status < 0) {
        action = CURL_CSELECT_ERR;
    }
    if (status == 0) {
        if ((events & UV_READABLE) != 0) {
            action |= CURL_CSELECT_IN;
        }
        if ((events & UV_WRITABLE) != 0) {
            action |= CURL_CSELECT_OUT;
        }
    }

    event_loop.checkActions(curl_context->GetCurlSockFd(), action);
}

auto requests_accept_async(uv_async_t* handle) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);
    
    /**
     * This lock must not have any "curl_*" functions called
     * while it is held, curl has its own internal locks and
     * it can cause a deadlock.  This means we intentionally swap
     * vectors before working on them so we have exclusive access
     * to the Request objects on the EventLoop thread.
     */
    {
        std::lock_guard<std::mutex> guard { event_loop->m_pending_requests_lock };
        // swap so we can release the lock as quickly as possible
        event_loop->m_grabbed_requests.swap(event_loop->m_pending_requests);
    }

    for (auto& request_handle : event_loop->m_grabbed_requests)
    {
        auto& raw_request = *request_handle;
        
        // If there's a response wait time, we'll need to create a ResponseWaitTimeWrapper and add
        // it to the wrapper multiset so it can be handled in the event it takes too long to respond.
        if (const auto& response_wait_time_opt = raw_request.GetResponseWaitTime(); response_wait_time_opt.has_value())
        {
            // Get the current timepoint from libuv (it caches the current timepoint at the beginning
            // of every cycle through the event loop, so this should not be expensive).
            auto time = uv_now(event_loop->m_loop);
            
            const auto request_timeout = response_wait_time_opt.value();
            auto next_timepoint = time + static_cast<uint64_t>(request_timeout.count());
        
            auto& wrappers = event_loop->m_response_wait_time_wrappers;
            
            if (wrappers.empty() || next_timepoint < wrappers.begin()->GetData().m_timeout_time)
            {
                uv_timer_stop(&event_loop->m_response_timer);
    
                uv_timer_start(
                    &event_loop->m_response_timer,
                    on_response_wait_time_expired_callback,
                    static_cast<uint64_t>(request_timeout.count()),
                    0);
            }
        
            auto iterator = event_loop->m_response_wait_time_wrappers.emplace(next_timepoint, request_handle.createSharedRequestOnHeap());
            request_handle->setTimeoutIterator(iterator);
        }
    
        // Put the shared_ptr to the SharedRequest on the heap and set curl handle's data to point to the pointer,
        // so its lifetime is maintained even after exiting this function
        raw_request.setSharedPointerOnCurlHandle(request_handle.createSharedRequestOnHeap());
        
        auto curl_code = curl_multi_add_handle(event_loop->m_cmh, raw_request.m_curl_handle);
    
        if(curl_code != CURLM_OK && curl_code != CURLM_CALL_MULTI_PERFORM)
        {
            /**
             * If curl_multi_add_handle fails then notify the user that the request failed to start
             * immediately.
             */
            request_handle->setCompletionStatus(CURLcode::CURLE_SEND_ERROR);
            request_handle->onComplete(*event_loop, *request_handle.createSharedRequestOnHeap());
        }
        else
        {
            
            
            /**
             * Immediately call curl's check action to get the current request moving.
             * Curl appears to have an internal queue and if it gets too long it might
             * drop requests.
             */
            event_loop->checkActions();
        }
    }
    
    event_loop->m_active_request_count += event_loop->m_grabbed_requests.size();
    event_loop->m_grabbed_requests.clear();
}

auto on_response_wait_time_expired_callback(uv_timer_t* handle) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);
    event_loop->stopTimedOutRequests();
}


} // lift
