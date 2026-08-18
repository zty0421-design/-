#include "trpg/Config.h"
#include "trpg/CoreService.h"
#include "trpg/RoomSocket.h"

#include <drogon/drogon.h>

#include <exception>
#include <iostream>
#include <memory>

int main() {
  try {
    // [啟動功能備註｜環境設定] 讀取 PORT、DATABASE_URL、JWT、DM 帳號等部署設定。
    const auto config = trpg::Config::fromEnvironment();
    auto service = std::make_shared<trpg::CoreService>(config);
    trpg::CoreService::setInstance(service);

    // [啟動功能備註｜資料庫] 建立／遷移 PostgreSQL schema；改資料表時先看 migration。
    service->initializeDatabase();
    // [啟動功能備註｜HTTP API] 註冊核心 C++ 路由與 v39 相容 API。
    service->registerRoutes();

    // [啟動功能備註｜前端靜態檔] 提供 index、PWA manifest、Service Worker 與圖示。
    drogon::app().setDocumentRoot(config.documentRoot);
    drogon::app().setHomePage("index.html");
    // Drogon only serves whitelisted static extensions. PWA manifests use
    // .webmanifest, which is not in the default whitelist. Keep the normal
    // browser assets explicit so Render serves the full PWA shell.
    drogon::app().setFileTypes({
        "html", "css", "js", "png", "jpg", "jpeg", "gif", "ico",
        "svg", "xml", "webmanifest"});
    drogon::app().registerCustomExtensionMime(
        "webmanifest", "application/manifest+json");
    drogon::app().setThreadNum(config.httpThreads);
    drogon::app().setClientMaxBodySize(2U * 1024U * 1024U);
    drogon::app().addListener("0.0.0.0", config.port);
    // [啟動功能備註｜安全標頭] 全站 HTTP 回應共用安全標頭；若要放寬 iframe/權限從這裡改。
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
