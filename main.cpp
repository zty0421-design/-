#include "trpg/Config.h"
#include "trpg/CoreService.h"
#include "trpg/RoomSocket.h"

#include <drogon/drogon.h>

#include <exception>
#include <iostream>
#include <memory>

int main() {
  try {
    const auto config = trpg::Config::fromEnvironment();
    auto service = std::make_shared<trpg::CoreService>(config);
    trpg::CoreService::setInstance(service);

    service->initializeDatabase();
    service->registerRoutes();

    drogon::app().setDocumentRoot(config.documentRoot);
    drogon::app().setHomePage("index.html");
    drogon::app().setThreadNum(config.httpThreads);
    drogon::app().setClientMaxBodySize(2U * 1024U * 1024U);
    drogon::app().addListener("0.0.0.0", config.port);
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& response) {
          response->addHeader("X-Content-Type-Options", "nosniff");
          response->addHeader("X-Frame-Options", "DENY");
          response->addHeader("Referrer-Policy", "same-origin");
          response->addHeader(
              "Permissions-Policy",
              "camera=(), microphone=(), geolocation=(), payment=()");
        });

    LOG_INFO << "TRPG C++ " << trpg::CoreService::kVersion
             << " listening on 0.0.0.0:" << config.port;
    drogon::app().run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Fatal startup error: " << error.what() << '\n';
    return 1;
  }
}
