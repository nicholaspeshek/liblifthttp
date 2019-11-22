#include <lift/Lift.h>

#include <iostream>

static auto on_complete(lift::RequestHandle request_ptr) -> void
{
    auto& request = *request_ptr;
    switch (request.GetCompletionStatus()) {
        case lift::RequestStatus::SUCCESS:
            std::cout
                << "Completed " << request.GetUrl()
                << " ms:" << request.GetTotalTime().count() << std::endl;
            break;
        case lift::RequestStatus::CONNECT_ERROR:
            std::cout << "Unable to connect to: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::CONNECT_DNS_ERROR:
            std::cout << "Unable to lookup DNS entry for: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::CONNECT_SSL_ERROR:
            std::cout << "SSL Error for: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::TIMEOUT:
            std::cout << "Timeout: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::REQUEST_TIMEOUT:
            std::cout << "Request Timeout: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::RESPONSE_EMPTY:
            std::cout << "No response received: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::DOWNLOAD_ERROR:
            std::cout << "Error occurred in CURL write callback: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::ERROR_FAILED_TO_START:
            std::cout << "Error trying to start a request: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::ERROR:
            std::cout << "RequestHandle had an unrecoverable error: " << request.GetUrl() << std::endl;
            break;
        case lift::RequestStatus::BUILDING:
        case lift::RequestStatus::EXECUTING:
            std::cout
                << "RequestHandle is in an invalid state: "
                << to_string(request.GetCompletionStatus()) << std::endl;
            break;
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cout << "Please include host and data." << std::endl;
        return 0;
    }

    using namespace std::chrono_literals;
    std::string host{argv[1]};
    std::string data{argv[2]};

    // Initialize must be called first before using the LiftHttp library.
    lift::GlobalScopeInitializer lift_init {};

    lift::EventLoop event_loop;
    auto& request_pool = event_loop.GetRequestPool();

    auto count = 0;

    while (count < 100)
    {
        ++count;

        auto request = request_pool.Produce(host, on_complete, 60'000ms);
        request->SetRequestData(data);
        request->SetMethod(lift::http::Method::POST);
        request->SetFollowRedirects(true);
        request->SetVersion(lift::http::Version::V1_1);
//        request->AddHeader("Expect", "");

        /**
         * This will 'move' all of the Request objects into the event loop.
         * The values in the 'requests' vector are now no longer valid.  This
         * example intentionally has 'requests' go out of scope to further
         * demonstrate this.
         */
        event_loop.StartRequest(std::move(request));
    }

    std::this_thread::sleep_for(100ms); // just to be sure still gets kicked off

    // Now wait for all the requests to finish before cleaning up.
    while (event_loop.HasUnfinishedRequests()) {
        std::this_thread::sleep_for(100ms);
    }

    return 0;
}
